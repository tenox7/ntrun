/* cpu_ppc.c: 32-bit PowerPC user mode, little endian, as Windows NT uses it.
   A function pointer is the address of a descriptor: two words, the code address and the TOC (r2). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include "ntrun.h"

#if defined(_WIN32) && !defined(_UCRT)
/* the fma of msvcrt.dll (MINGW64) is not exactly rounded: the multiply-add of the PowerPC is */
#include "softfma.h"
#define fma soft_fma
#endif

static struct {
    u32 r[32];
    u64 f[32];
    u32 pc, lr, ctr, xer, cr, fpscr;
} P;

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

static inline double fd(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static inline u64 df(double d) { u64 v; memcpy(&v, &d, 8); return v; }

static void ppc_round(void)
{
    static const int mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(mode[P.fpscr & 3]);
}

/* a trap instruction that fired: "trap if equal to 0" is the check before a division */
static u32 ppc_trap_code(u32 i)
{
    if ((i >> 26) == 3 && ((i >> 21) & 4) && (i & 0xffff) == 0 && ((i >> 21) & 31) != 31)
        return STATUS_INTEGER_DIVIDE_BY_ZERO;
    return STATUS_BREAKPOINT;
}

static void ppc_dump(void)
{
    fprintf(stderr, "pc=%08x lr=%08x ctr=%08x cr=%08x xer=%08x\n", P.pc, P.lr, P.ctr, P.cr, P.xer);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "r%-2d=%08x%s", i, P.r[i], (i & 7) == 7 ? "\n" : " ");
}

static inline void crf(int bf, u32 c) { int sh = 28 - 4 * bf; P.cr = (P.cr & ~(0xfu << sh)) | (c << sh); }
static inline void cr0(u32 v) { crf(0, ((s32)v < 0 ? 8u : (s32)v > 0 ? 4u : 2u) | (P.xer >> 31)); }
static inline u32 rotl(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 mask(u32 mb, u32 me)
{
    u32 a = 0xffffffffu >> mb, b = 0xffffffffu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
static inline void setov(int ov)
{
    if (ov)
        P.xer |= XER_OV | XER_SO;
    else
        P.xer &= ~XER_OV;
}
static inline void setca(int c) { P.xer = c ? (P.xer | XER_CA) : (P.xer & ~XER_CA); }

static void ppc_run(void)
{
    u32 *r = P.r;
    u32 pc = P.pc;

    for (;;) {
        u32 i = r32(pc);
        u32 rt = (i >> 21) & 31, ra = (i >> 16) & 31, rb = (i >> 11) & 31, xo = (i >> 1) & 0x3ff;
        s32 simm = (s16)i;
        u32 uimm = (u16)i, ea, npc = pc + 4;
        int rc = i & 1;

        icount++;
        P.pc = pc;
        if (trace_insn)
            fprintf(stderr, "%08x: %08x\n", pc, i);
#define EA_D() (ea = (ra ? r[ra] : 0) + (u32)simm)
#define EA_DU() (ea = r[ra] + (u32)simm, r[ra] = ea)
#define EA_X() (ea = (ra ? r[ra] : 0) + r[rb])
#define EA_XU() (ea = r[ra] + r[rb], r[ra] = ea)
        switch (i >> 26) {
        case 3: { /* twi */
            s32 a = (s32)r[ra];
            if (((rt & 16) && a < simm) || ((rt & 8) && a > simm) || ((rt & 4) && a == simm) ||
                ((rt & 2) && (u32)a < (u32)simm) || ((rt & 1) && (u32)a > (u32)simm))
                cpu_fault(ppc_trap_code(i), pc, i);
            break;
        }
        case 7: r[rt] = (u32)((s32)r[ra] * simm); break;
        case 8: { u64 t = (u64)(u32)~r[ra] + (u32)simm + 1; r[rt] = (u32)t; setca((int)(t >> 32)); break; }
        case 10: { u32 a = r[ra]; crf((int)(rt >> 2), (a < uimm ? 8u : a > uimm ? 4u : 2u) | (P.xer >> 31)); break; }
        case 11: { s32 a = (s32)r[ra]; crf((int)(rt >> 2), (a < simm ? 8u : a > simm ? 4u : 2u) | (P.xer >> 31)); break; }
        case 12: { u64 t = (u64)r[ra] + (u32)simm; r[rt] = (u32)t; setca((int)(t >> 32)); break; }
        case 13: { u64 t = (u64)r[ra] + (u32)simm; r[rt] = (u32)t; setca((int)(t >> 32)); cr0(r[rt]); break; }
        case 14: r[rt] = (ra ? r[ra] : 0) + (u32)simm; break;
        case 15: r[rt] = (ra ? r[ra] : 0) + ((u32)simm << 16); break;
        case 16: { /* bc */
            u32 bo = rt, bi = ra;
            int ok = 1;
            if (!(bo & 4)) {
                P.ctr--;
                ok = (P.ctr != 0) ^ ((bo >> 1) & 1);
            }
            if (ok && !(bo & 16))
                ok = ((P.cr >> (31 - bi)) & 1) == ((bo >> 3) & 1);
            if (i & 1)
                P.lr = pc + 4;
            if (ok)
                npc = ((i & 2) ? 0 : pc) + (u32)(s32)(s16)(i & 0xfffc);
            break;
        }
        case 17: /* sc: only our thunks have it */
            if (pc >= THUNK_BASE && pc < THUNK_BASE + THUNK_MAX * THUNK_SLOT) {
                u32 idx = (pc - THUNK_BASE) / THUNK_SLOT, a[16], ret = P.lr, sp = r[1];
                if (idx == API_RETURN)
                    return;
                for (int k = 0; k < 8; k++) a[k] = r[3 + k];
                for (int k = 8; k < 16; k++) a[k] = r32(sp + 24 + 4 * (u32)k);
                P.pc = ret;
                r[3] = api_dispatch(idx, a);
                npc = ret;
                break;
            }
            cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
        case 18: { /* b */
            s32 li = (s32)(i << 6) >> 6 & ~3;
            if (i & 1)
                P.lr = pc + 4;
            npc = ((i & 2) ? 0 : pc) + (u32)li;
            break;
        }
        case 19:
            switch (xo) {
            case 0: { u32 c = (P.cr >> (28 - 4 * ((i >> 18) & 7))) & 15; crf((int)(rt >> 2), c); break; }
            case 16: case 528: {
                u32 bo = rt, bi = ra, t = xo == 16 ? P.lr : P.ctr;
                int ok = 1;
                if (!(bo & 4) && xo == 16) {
                    P.ctr--;
                    ok = (P.ctr != 0) ^ ((bo >> 1) & 1);
                }
                if (ok && !(bo & 16))
                    ok = ((P.cr >> (31 - bi)) & 1) == ((bo >> 3) & 1);
                if (i & 1)
                    P.lr = pc + 4;
                if (ok)
                    npc = t & ~3u;
                break;
            }
            case 150: break; /* isync */
            default: {
                u32 a = (P.cr >> (31 - ra)) & 1, b = (P.cr >> (31 - rb)) & 1, v;
                switch (xo) {
                case 257: v = a & b; break;
                case 449: v = a | b; break;
                case 193: v = a ^ b; break;
                case 225: v = !(a & b); break;
                case 33: v = !(a | b); break;
                case 289: v = !(a ^ b); break;
                case 129: v = a & !b; break;
                case 417: v = a | !b; break;
                default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                P.cr = (P.cr & ~(0x80000000u >> rt)) | (v << (31 - rt));
            }
            }
            break;
        case 20: { u32 m = mask((i >> 6) & 31, (i >> 1) & 31); r[ra] = (rotl(r[rt], rb) & m) | (r[ra] & ~m); if (rc) cr0(r[ra]); break; }
        case 21: r[ra] = rotl(r[rt], rb) & mask((i >> 6) & 31, (i >> 1) & 31); if (rc) cr0(r[ra]); break;
        case 23: r[ra] = rotl(r[rt], r[rb]) & mask((i >> 6) & 31, (i >> 1) & 31); if (rc) cr0(r[ra]); break;
        case 24: r[ra] = r[rt] | uimm; break;
        case 25: r[ra] = r[rt] | (uimm << 16); break;
        case 26: r[ra] = r[rt] ^ uimm; break;
        case 27: r[ra] = r[rt] ^ (uimm << 16); break;
        case 28: r[ra] = r[rt] & uimm; cr0(r[ra]); break;
        case 29: r[ra] = r[rt] & (uimm << 16); cr0(r[ra]); break;
        case 31: {
            u32 a = r[ra], b = r[rb], x9 = xo & 0x1ff, res;
            int oe = (int)(xo >> 9);
            switch (xo) {
            case 0: { s32 x = (s32)a, y = (s32)b; crf((int)(rt >> 2), (x < y ? 8u : x > y ? 4u : 2u) | (P.xer >> 31)); goto done31; }
            case 32: crf((int)(rt >> 2), (a < b ? 8u : a > b ? 4u : 2u) | (P.xer >> 31)); goto done31;
            case 4: {
                s32 x = (s32)a, y = (s32)b;
                if (((rt & 16) && x < y) || ((rt & 8) && x > y) || ((rt & 4) && x == y) || ((rt & 2) && a < b) || ((rt & 1) && a > b))
                    cpu_fault(ppc_trap_code(i), pc, i);
                goto done31;
            }
            case 19: r[rt] = P.cr; goto done31;
            case 20: EA_X(); r[rt] = r32(ea); goto done31;
            case 23: EA_X(); r[rt] = r32(ea); goto done31;
            case 55: EA_XU(); r[rt] = r32(ea); goto done31;
            case 87: EA_X(); r[rt] = r8(ea); goto done31;
            case 119: EA_XU(); r[rt] = r8(ea); goto done31;
            case 279: EA_X(); r[rt] = r16(ea); goto done31;
            case 311: EA_XU(); r[rt] = r16(ea); goto done31;
            case 343: EA_X(); r[rt] = (u32)(s32)(s16)r16(ea); goto done31;
            case 375: EA_XU(); r[rt] = (u32)(s32)(s16)r16(ea); goto done31;
            case 150: EA_X(); w32(ea, r[rt]); crf(0, 2u | (P.xer >> 31)); goto done31;
            case 151: EA_X(); w32(ea, r[rt]); goto done31;
            case 183: { u32 v = r[rt]; EA_XU(); w32(ea, v); goto done31; }
            case 215: EA_X(); w8(ea, (u8)r[rt]); goto done31;
            case 247: { u32 v = r[rt]; EA_XU(); w8(ea, (u8)v); goto done31; }
            case 407: EA_X(); w16(ea, (u16)r[rt]); goto done31;
            case 439: { u32 v = r[rt]; EA_XU(); w16(ea, (u16)v); goto done31; }
            case 534: EA_X(); r[rt] = __builtin_bswap32(r32(ea)); goto done31;
            case 790: EA_X(); r[rt] = __builtin_bswap16(r16(ea)); goto done31;
            case 662: EA_X(); w32(ea, __builtin_bswap32(r[rt])); goto done31;
            case 918: EA_X(); w16(ea, __builtin_bswap16((u16)r[rt])); goto done31;
            case 535: { float f; EA_X(); u32 v = r32(ea); memcpy(&f, &v, 4); P.f[rt] = df((double)f); goto done31; }
            case 567: { float f; EA_XU(); u32 v = r32(ea); memcpy(&f, &v, 4); P.f[rt] = df((double)f); goto done31; }
            case 599: EA_X(); P.f[rt] = r64(ea); goto done31;
            case 631: EA_XU(); P.f[rt] = r64(ea); goto done31;
            case 663: { float f = (float)fd(P.f[rt]); u32 v; memcpy(&v, &f, 4); EA_X(); w32(ea, v); goto done31; }
            case 695: { float f = (float)fd(P.f[rt]); u32 v; memcpy(&v, &f, 4); EA_XU(); w32(ea, v); goto done31; }
            case 727: EA_X(); w64(ea, P.f[rt]); goto done31;
            case 759: EA_XU(); w64(ea, P.f[rt]); goto done31;
            case 983: EA_X(); w32(ea, (u32)P.f[rt]); goto done31;
            case 339: {
                u32 spr = ra | (rb << 5);
                r[rt] = spr == 1 ? P.xer : spr == 8 ? P.lr : spr == 9 ? P.ctr : 0;
                if (spr != 1 && spr != 8 && spr != 9)
                    cpu_fault(0xc0000096, pc, i);
                goto done31;
            }
            case 467: {
                u32 spr = ra | (rb << 5);
                if (spr == 1) P.xer = r[rt];
                else if (spr == 8) P.lr = r[rt];
                else if (spr == 9) P.ctr = r[rt];
                else cpu_fault(0xc0000096, pc, i);
                goto done31;
            }
            case 371: r[rt] = (u32)icount; goto done31;
            case 144: {
                u32 fxm = (i >> 12) & 0xff, m = 0;
                for (int k = 0; k < 8; k++)
                    if (fxm & (0x80u >> k))
                        m |= 0xf0000000u >> (4 * k);
                P.cr = (P.cr & ~m) | (r[rt] & m);
                goto done31;
            }
            case 512: crf((int)(rt >> 2), P.xer >> 28); P.xer &= 0x0fffffffu; goto done31;
            case 54: case 86: case 246: case 278: case 982: case 598: case 854: goto done31;
            case 1014: EA_X(); memset(G + (ea & ~31u), 0, 32); goto done31;
            /* logical: the result goes to ra */
            case 28: res = r[rt] & b; break;
            case 60: res = r[rt] & ~b; break;
            case 444: res = r[rt] | b; break;
            case 412: res = r[rt] | ~b; break;
            case 316: res = r[rt] ^ b; break;
            case 476: res = ~(r[rt] & b); break;
            case 124: res = ~(r[rt] | b); break;
            case 284: res = ~(r[rt] ^ b); break;
            case 954: res = (u32)(s32)(s8)r[rt]; break;
            case 922: res = (u32)(s32)(s16)r[rt]; break;
            case 26: res = r[rt] ? (u32)__builtin_clz(r[rt]) : 32; break;
            case 24: res = (b & 32) ? 0 : r[rt] << (b & 31); break;
            case 536: res = (b & 32) ? 0 : r[rt] >> (b & 31); break;
            case 792: {
                s32 s = (s32)r[rt];
                u32 n = b & 63;
                if (n > 31) {
                    res = (u32)(s >> 31);
                    setca(s < 0);
                } else {
                    res = (u32)(s >> n);
                    setca(s < 0 && n && (r[rt] & ((1u << n) - 1)));
                }
                break;
            }
            case 824: {
                s32 s = (s32)r[rt];
                res = (u32)(s >> rb);
                setca(s < 0 && rb && (r[rt] & ((1u << rb) - 1)));
                break;
            }
            default:
                /* arithmetic: the result goes to rt; bit 9 of xo is OE */
                switch (x9) {
                case 8: { u64 t = (u64)(u32)~a + b + 1; res = (u32)t; setca((int)(t >> 32)); if (oe) setov((int)(((~a ^ res) & (b ^ res)) >> 31)); break; }
                case 10: { u64 t = (u64)a + b; res = (u32)t; setca((int)(t >> 32)); if (oe) setov((int)(((a ^ res) & (b ^ res)) >> 31)); break; }
                case 40: res = b - a; if (oe) setov((int)(((~a ^ res) & (b ^ res)) >> 31)); break;
                case 104: res = (u32)-(s32)a; if (oe) setov(a == 0x80000000u); break;
                case 136: { u64 t = (u64)(u32)~a + b + ((P.xer >> 29) & 1); res = (u32)t; setca((int)(t >> 32)); if (oe) setov((int)(((~a ^ res) & (b ^ res)) >> 31)); break; }
                case 138: { u64 t = (u64)a + b + ((P.xer >> 29) & 1); res = (u32)t; setca((int)(t >> 32)); if (oe) setov((int)(((a ^ res) & (b ^ res)) >> 31)); break; }
                case 200: { u64 t = (u64)(u32)~a + ((P.xer >> 29) & 1); res = (u32)t; setca((int)(t >> 32)); break; }
                case 202: { u64 t = (u64)a + ((P.xer >> 29) & 1); res = (u32)t; setca((int)(t >> 32)); break; }
                case 232: { u64 t = (u64)(u32)~a + ((P.xer >> 29) & 1) + 0xffffffffu; res = (u32)t; setca((int)(t >> 32)); break; }
                case 234: { u64 t = (u64)a + ((P.xer >> 29) & 1) + 0xffffffffu; res = (u32)t; setca((int)(t >> 32)); break; }
                case 235: { s64 t = (s64)(s32)a * (s32)b; res = (u32)t; if (oe) setov(t != (s64)(s32)t); break; }
                case 266: res = a + b; if (oe) setov((int)(((a ^ res) & (b ^ res)) >> 31)); break;
                case 75: res = (u32)(((s64)(s32)a * (s32)b) >> 32); break;
                case 11: res = (u32)(((u64)a * b) >> 32); break;
                case 459: res = b ? a / b : 0; if (oe) setov(!b); break;
                case 491:
                    if (!b || (a == 0x80000000u && b == 0xffffffffu)) {
                        res = 0;
                        if (oe) setov(1);
                    } else {
                        res = (u32)((s32)a / (s32)b);
                        if (oe) setov(0);
                    }
                    break;
                default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                r[rt] = res;
                if (rc)
                    cr0(res);
                goto done31;
            }
            r[ra] = res;
            if (rc)
                cr0(res);
        done31:
            break;
        }
        case 32: EA_D(); r[rt] = r32(ea); break;
        case 33: EA_DU(); r[rt] = r32(ea); break;
        case 34: EA_D(); r[rt] = r8(ea); break;
        case 35: EA_DU(); r[rt] = r8(ea); break;
        case 36: EA_D(); w32(ea, r[rt]); break;
        case 37: { u32 v = r[rt]; EA_DU(); w32(ea, v); break; }
        case 38: EA_D(); w8(ea, (u8)r[rt]); break;
        case 39: { u32 v = r[rt]; EA_DU(); w8(ea, (u8)v); break; }
        case 40: EA_D(); r[rt] = r16(ea); break;
        case 41: EA_DU(); r[rt] = r16(ea); break;
        case 42: EA_D(); r[rt] = (u32)(s32)(s16)r16(ea); break;
        case 43: EA_DU(); r[rt] = (u32)(s32)(s16)r16(ea); break;
        case 44: EA_D(); w16(ea, (u16)r[rt]); break;
        case 45: { u32 v = r[rt]; EA_DU(); w16(ea, (u16)v); break; }
        case 46: EA_D(); for (u32 k = rt; k < 32; k++, ea += 4) r[k] = r32(ea); break;
        case 47: EA_D(); for (u32 k = rt; k < 32; k++, ea += 4) w32(ea, r[k]); break;
        case 48: { float f; u32 v; EA_D(); v = r32(ea); memcpy(&f, &v, 4); P.f[rt] = df((double)f); break; }
        case 49: { float f; u32 v; EA_DU(); v = r32(ea); memcpy(&f, &v, 4); P.f[rt] = df((double)f); break; }
        case 50: EA_D(); P.f[rt] = r64(ea); break;
        case 51: EA_DU(); P.f[rt] = r64(ea); break;
        case 52: { float f = (float)fd(P.f[rt]); u32 v; memcpy(&v, &f, 4); EA_D(); w32(ea, v); break; }
        case 53: { float f = (float)fd(P.f[rt]); u32 v; memcpy(&v, &f, 4); EA_DU(); w32(ea, v); break; }
        case 54: EA_D(); w64(ea, P.f[rt]); break;
        case 55: EA_DU(); w64(ea, P.f[rt]); break;
        case 59:
        case 63: {
            int single = (i >> 26) == 59;
            u32 x5 = (i >> 1) & 31, fc = (i >> 6) & 31;
            double a = fd(P.f[ra]), b = fd(P.f[rb]), c = fd(P.f[fc]), res;
            int innan = 0; /* an operand that the instruction uses is a NaN */
            if (x5 >= 18 && x5 != 19 && x5 != 24 + (single ? 99 : 0) && x5 != 27) {
                switch (x5) {
                case 18: res = a / b; innan = isnan(a) || isnan(b); break;
                case 20: res = a - b; innan = isnan(a) || isnan(b); break;
                case 21: res = a + b; innan = isnan(a) || isnan(b); break;
                case 22: res = sqrt(b); innan = isnan(b); break;
                case 23: res = a >= 0.0 ? c : b; innan = 1; break; /* a select: no new NaN */
                case 24: res = 1.0 / b; innan = isnan(b); break;
                case 25: res = a * c; innan = isnan(a) || isnan(c); break;
                case 26: res = 1.0 / sqrt(b); innan = isnan(b); break;
                case 28: res = fma(a, c, -b); innan = isnan(a) || isnan(b) || isnan(c); break;
                case 29: res = fma(a, c, b); innan = isnan(a) || isnan(b) || isnan(c); break;
                case 30: res = -fma(a, c, -b); innan = isnan(a) || isnan(b) || isnan(c); break;
                case 31: res = -fma(a, c, b); innan = isnan(a) || isnan(b) || isnan(c); break;
                default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                /* an invalid operation gives the default NaN of the PowerPC (positive); an x86 host gives
                   a negative one. Only the operands that the instruction uses count: the others are f0. */
                if (isnan(res) && !innan)
                    res = __builtin_nan("");
                if (single)
                    res = (double)(float)res;
                P.f[rt] = df(res);
                break;
            }
            if (single)
                cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            switch (xo) {
            case 0: case 32: {
                u32 cc = (isnan(a) || isnan(b)) ? 1u : a < b ? 8u : a > b ? 4u : 2u;
                crf((int)(rt >> 2), cc);
                P.fpscr = (P.fpscr & ~0xf000u) | (cc << 12);
                break;
            }
            case 12: P.f[rt] = df((double)(float)b); break;
            case 14: case 15: {
                double x = xo == 15 ? trunc(b) : nearbyint(b);
                s32 v = isnan(b) ? INT32_MIN : x >= 2147483648.0 ? INT32_MAX : x < -2147483648.0 ? INT32_MIN : (s32)x;
                P.f[rt] = 0xfff8000000000000ull | (u32)v;
                break;
            }
            case 40: P.f[rt] = P.f[rb] ^ 0x8000000000000000ull; break;
            case 72: P.f[rt] = P.f[rb]; break;
            case 136: P.f[rt] = P.f[rb] | 0x8000000000000000ull; break;
            case 264: P.f[rt] = P.f[rb] & 0x7fffffffffffffffull; break;
            case 583: P.f[rt] = 0xfff8000000000000ull | P.fpscr; break;
            case 711: {
                u32 fm = (i >> 17) & 0xff, m = 0;
                for (int k = 0; k < 8; k++)
                    if (fm & (0x80u >> k))
                        m |= 0xf0000000u >> (4 * k);
                P.fpscr = (P.fpscr & ~m) | ((u32)P.f[rb] & m);
                ppc_round();
                break;
            }
            case 134: { int sh = 28 - 4 * (int)(rt >> 2); P.fpscr = (P.fpscr & ~(0xfu << sh)) | (((i >> 12) & 15) << sh); ppc_round(); break; }
            case 70: P.fpscr &= ~(0x80000000u >> rt); ppc_round(); break;
            case 38: P.fpscr |= 0x80000000u >> rt; ppc_round(); break;
            case 64: crf((int)(rt >> 2), (P.fpscr >> (28 - 4 * ((i >> 18) & 7))) & 15); break;
            default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            }
            break;
        }
        default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
        }
        pc = npc;
    }
}

static void ppc_init(u32 sp, u32 teb, u32 gp)
{
    (void)gp;
    memset(&P, 0, sizeof P);
    P.r[1] = (sp - 128) & ~15u;
    P.r[13] = teb;
    P.lr = THUNK_BASE;
    ppc_round();
}

static void ppc_thunk(u32 addr)
{
    w32(addr, 0x44000002);     /* sc */
    w32(addr + 4, 0x4e800020); /* blr (never runs) */
    w32(addr + 8, addr);       /* the descriptor: code address, TOC */
    w32(addr + 12, 0);
}

static u32 ppc_thunk_ptr(u32 addr) { return addr + 8; }

/* fn is the address of a function descriptor */
static void ppc_call_begin(u32 fn, int n, const u32 *args, u64 *save)
{
    u32 sp0 = P.r[1], sp = (sp0 - 256) & ~15u;

    save[0] = P.lr;
    save[1] = sp0;
    save[2] = P.pc;
    save[3] = P.r[2];
    w32(sp, sp0);
    for (int k = 0; k < n; k++) {
        if (k < 8) P.r[3 + k] = args[k];
        else w32(sp + 24 + 4 * (u32)k, args[k]);
    }
    P.r[1] = sp;
    P.lr = THUNK_BASE;
    P.r[2] = r32(fn + 4);
    P.pc = r32(fn);
}

static u32 ppc_call_end(const u64 *save)
{
    u32 ret = P.r[3];
    P.lr = (u32)save[0];
    P.r[1] = (u32)save[1];
    P.pc = (u32)save[2];
    P.r[2] = (u32)save[3];
    return ret;
}

static u32 ppc_pc(void) { return P.pc; }
static u32 ppc_sp(void) { return P.r[1]; }
static u32 ppc_ra(void) { return P.lr; }
static void ppc_set_sp(u32 sp) { P.r[1] = sp; }

/* ---- exceptions: the CONTEXT of Windows NT for PowerPC ---- */

#define PX_FPR 0x000
#define PX_FPSCR 0x100
#define PX_GPR 0x108
#define PX_CR 0x188
#define PX_XER 0x18c
#define PX_MSR 0x190
#define PX_IAR 0x194
#define PX_LR 0x198
#define PX_CTR 0x19c
#define PX_FLAGS 0x1a0
#define PX_SIZE 0x1d0
#define GR(c, n) ((c) + PX_GPR + 4 * (n))
#define FPR(c, n) ((c) + PX_FPR + 8 * (n))

static void ppc_capture(u32 c, u32 fir)
{
    memset(G + c, 0, PX_SIZE);
    for (int i = 0; i < 32; i++) {
        w64(FPR(c, i), P.f[i]);
        w32(GR(c, i), P.r[i]);
    }
    w64(c + PX_FPSCR, 0xfff8000000000000ull | P.fpscr);
    w32(c + PX_CR, P.cr);
    w32(c + PX_XER, P.xer);
    w32(c + PX_MSR, 0x0000f031);
    w32(c + PX_IAR, fir);
    w32(c + PX_LR, P.lr);
    w32(c + PX_CTR, P.ctr);
    w32(c + PX_FLAGS, 7);
}

static void ppc_restore(u32 c)
{
    for (int i = 0; i < 32; i++) {
        P.f[i] = r64(FPR(c, i));
        P.r[i] = r32(GR(c, i));
    }
    P.fpscr = (u32)r64(c + PX_FPSCR);
    ppc_round();
    P.cr = r32(c + PX_CR);
    P.xer = r32(c + PX_XER);
    P.lr = r32(c + PX_LR);
    P.ctr = r32(c + PX_CTR);
    P.pc = r32(c + PX_IAR);
}

static u32 ppc_lookup(u32 pc)
{
    u32 size, tab = find_pdata(pc, &size);
    s32 lo = 0, hi = (s32)(size / 20) - 1;

    if (!tab)
        return 0;
    while (hi >= lo) {
        s32 mid = (lo + hi) >> 1;
        u32 fe = tab + (u32)mid * 20;
        if (pc < r32(fe))
            hi = mid - 1;
        else if (pc >= r32(fe + 4))
            lo = mid + 1;
        else {
            u32 pe = r32(fe + 16);
            if ((pe < r32(fe) || pe > r32(fe + 4)) && (pe & 3) == 0)
                fe = pe; /* a second entry for the same function: go to the first */
            return fe;
        }
    }
    return 0;
}

/*
 * The prologue of a PowerPC function can call a small routine that saves registers
 * ("millicode"), and calls to other modules go through glue code. The function table
 * marks those with no handler and handler data 1 (save), 2 (restore) or 3 (glue).
 * So the unwinder reads instructions with one of four intents and keeps a small
 * stack of address ranges to read.
 */
enum { I_IGNORE, I_MFLR, I_MFCR, I_STW, I_STWU, I_STWUR12, I_STFD, I_MR, I_MRR12, I_MRFWD, I_ADDIR12, I_ADDIFWD,
    I_SAVE, I_RESTORE, I_GLUE, I_BLR, I_TOCRESTORE, I_SETEST };
enum { U_FWD, U_R12, U_REV, U_REVR12 };
struct milli { u32 target, fe; };

#define BRANCH_TARGET(i, pc) ((u32)(((s32)((i) << 6) >> 6) & ~3) + (((i) & 2) ? 0 : (pc)))

static int ppc_classify(u32 i, int intent, u32 pc, struct milli *info)
{
    u32 op = i >> 26, rs = (i >> 21) & 31, ra = (i >> 16) & 31, rb = (i >> 11) & 31, xo = (i >> 1) & 0x3ff;

    switch (op) {
    case 36: /* stw */
        if (intent == U_REVR12 && ra == 12)
            return I_STW;
        if ((intent == U_REVR12 || intent == U_REV) && ra == 1)
            return I_STW;
        break;
    case 32: /* lwz */
        if (intent == U_FWD)
            return I_STW;
        if (intent == U_REV) {
            if (ra == 1 && rs == 2)
                return I_TOCRESTORE;
            if (rs == 1 && ra == 1)
                return I_STWU;
        }
        break;
    case 37: /* stwu */
        if (intent != U_FWD && rs == 1 && ra == 1)
            return intent == U_R12 ? I_STWUR12 : I_STWU;
        break;
    case 54: /* stfd */
        if (intent == U_REVR12 && ra == 12)
            return I_STFD;
        if ((intent == U_REVR12 || intent == U_REV) && ra == 1)
            return I_STFD;
        break;
    case 50: /* lfd */
        if (intent == U_FWD)
            return I_STFD;
        break;
    case 14: /* addi */
        if (intent == U_R12 && rs == 12 && ra == 1)
            return I_ADDIR12;
        if (intent == U_FWD)
            return I_ADDIFWD;
        break;
    case 18: /* b, bl */
        if (intent == U_REV) {
            u32 t = BRANCH_TARGET(i, pc), fe;
            if (i & 1) {
                u32 ti = r32(t);
                if ((ti >> 26) == 31 && ((ti >> 1) & 0x3ff) == 339 && ((ti >> 11) & 0x3ff) == 0x100)
                    break; /* a call to code that starts with mflr: not millicode */
            }
            fe = ppc_lookup(t);
            if (fe && r32(fe + 8) == 0) {
                info->fe = fe;
                info->target = t;
                switch (r32(fe + 12)) {
                case 1:
                    if (i & 1)
                        return I_SAVE;
                    break;
                case 2:
                    if (!(i & 1))
                        return I_RESTORE;
                    break;
                case 3:
                    return I_GLUE;
                }
            }
        }
        break;
    case 19:
        if (intent == U_FWD) {
            if (i == 0x4e800020)
                return I_BLR;
        } else if (xo == 50) { /* rfi marks an exception frame */
            return I_SETEST;
        }
        break;
    case 31:
        switch (xo) {
        case 444: /* or: a move when the two sources are the same */
            if (rs != rb)
                break;
            if (intent == U_R12) {
                if (ra == 12 && rb == 1)
                    return I_MRR12;
            } else if (intent == U_FWD) {
                return I_MRFWD;
            } else if (rb != 1) {
                return I_MR;
            }
            break;
        case 183: /* stwux */
            if (intent != U_FWD && rs == 1 && ra == 1)
                return intent == U_R12 ? I_STWUR12 : I_STWU;
            break;
        case 339: /* mfspr */
            if (intent == U_REV && ((i >> 11) & 0x3ff) == 0x100)
                return I_MFLR;
            break;
        case 467: /* mtspr */
            if (intent == U_FWD && ((i >> 11) & 0x3ff) == 0x100)
                return I_MFLR;
            break;
        case 19: /* mfcr */
            if (intent == U_REV || intent == U_REVR12)
                return I_MFCR;
            break;
        case 144: /* mtcrf */
            if (intent == U_FWD && ((i >> 12) & 0xff) == 255)
                return I_MFCR;
            break;
        }
        break;
    }
    return I_IGNORE;
}

static u32 ppc_vunwind_ex(u32 pc, u32 fe, u32 c, int *infunc, u32 *estab, u32 ptrs, u32 lowlim, u32 highlim)
{
    struct { u32 begin, end; s32 inc; int intent; } it[8];
    struct milli info = { 0, 0 };
    u32 next, i, addr, rt, est = r32(GR(c, 1));
    int p, got_lr = 0, got_sp = 0, computed_sp = 0, cls;

    if (!fe) {
        next = r32(c + PX_LR) - 4;
        if (next == pc)
            return next;
        goto check_glue;
    }
    *estab = est;
    i = r32(pc);
    if (i == 0x4e800020) { /* at the return */
        *infunc = 0;
        next = r32(c + PX_LR);
        goto check_glue;
    }
    cls = ppc_classify(i, U_REV, pc, &info);
    if (cls == I_RESTORE) { /* about to branch to the restore millicode: run it */
        it[0].begin = info.target;
        it[0].end = r32(info.fe + 4);
        it[0].inc = 4;
        it[0].intent = U_FWD;
        *infunc = 0;
    } else if (r32(fe + 8) == 0 && r32(fe + 12) == 2) { /* in the restore millicode */
        it[0].begin = pc;
        it[0].end = r32(fe + 4);
        it[0].inc = 4;
        it[0].intent = U_FWD;
        *infunc = 0;
    } else {
        if (cls == I_TOCRESTORE && ppc_classify(r32(pc - 4), U_REV, pc - 4, &info) == I_GLUE) {
            /* back from a call through glue: the TOC is not restored yet. Do it. */
            pc += 4;
            addr = r32(GR(c, (i >> 16) & 31)) + (u32)(s32)(s16)i;
            rt = (i >> 21) & 31;
            w32(GR(c, rt), r32(addr));
            if (ptrs)
                w32(ptrs + 4 * 32 + 4 * rt, addr);
        }
        it[0].end = r32(fe) - 4;
        it[0].inc = -4;
        it[0].intent = U_REV;
        if (pc < r32(fe) || pc >= (r32(fe + 16) & ~3u)) {
            *infunc = 1;
            it[0].begin = (r32(fe + 16) & ~3u) - 4;
        } else {
            *infunc = 0;
            it[0].begin = pc - 4;
        }
    }
    next = r32(c + PX_LR) - 4;

unwind_glue:
    for (p = 0; p >= 0; p--) {
        u32 x;
        for (x = it[p].begin; x != it[p].end; x += (u32)it[p].inc) {
            i = r32(x);
            addr = r32(GR(c, (i >> 16) & 31)) + (u32)(s32)(s16)i;
            rt = (i >> 21) & 31;
            switch (ppc_classify(i, it[p].intent, x, &info)) {
            case I_MFLR:
                w32(c + PX_LR, r32(GR(c, rt)));
                if (!got_lr) {
                    next = r32(c + PX_LR) - 4;
                    got_lr = 1;
                } else {
                    next += 4;
                }
                continue;
            case I_BLR:
                next = r32(c + PX_LR) - 4;
                break;
            case I_MFCR:
                w32(c + PX_CR, r32(GR(c, rt)));
                continue;
            case I_STW:
                w32(GR(c, rt), r32(addr));
                if (ptrs)
                    w32(ptrs + 4 * 32 + 4 * rt, addr);
                continue;
            case I_STWU:
                addr = r32(GR(c, 1));
                w32(GR(c, 1), r32(addr));
                if (!got_sp) {
                    *estab = est = r32(GR(c, 1));
                    got_sp = 1;
                }
                if (ptrs)
                    w32(ptrs + 4 * 32 + 4 * rt, addr);
                continue;
            case I_STFD:
                w64(FPR(c, rt), r64(addr));
                if (ptrs)
                    w32(ptrs + 4 * rt, addr);
                continue;
            case I_MR:
                w32(GR(c, (i >> 16) & 31), r32(GR(c, rt)));
                continue;
            case I_MRFWD:
                w32(GR(c, rt), r32(GR(c, (i >> 16) & 31)));
                continue;
            case I_MRR12:
                w32(GR(c, rt), r32(GR(c, (i >> 16) & 31)));
                break;
            case I_ADDIFWD:
                w32(GR(c, rt), addr);
                continue;
            case I_ADDIR12:
                if (!computed_sp)
                    w32(GR(c, rt), r32(GR(c, 1)));
                w32(GR(c, rt), r32(GR(c, rt)) + (u32)(s32)(s16)i);
                break;
            case I_STWUR12:
                computed_sp = 1;
                addr = r32(GR(c, 1));
                w32(GR(c, 12), r32(addr));
                continue;
            case I_SAVE: /* a call to the save millicode: read it, and how r12 was made */
                if (p + 3 >= 8)
                    return 0;
                p++;
                it[p].begin = r32(info.fe + 4) - 4;
                it[p].end = info.target - 4;
                it[p].inc = -4;
                it[p].intent = U_REVR12;
                p++;
                it[p].begin = x - 4;
                it[p].end = it[p - 2].end;
                it[p].inc = -4;
                it[p].intent = U_R12;
                computed_sp = 0;
                it[p - 2].begin = x - 4;
                p++;
                break;
            case I_GLUE:
                if (p + 4 > 8)
                    return 0;
                p++;
                it[p].begin = (r32(info.fe + 16) & ~3u) - 4;
                it[p].end = r32(info.fe) - 4;
                it[p].inc = -4;
                it[p].intent = U_REV;
                it[p - 1].begin = x - 4;
                p++;
                break;
            case I_SETEST:
                *estab = est = r32(GR(c, 1));
                continue;
            default:
                continue;
            }
            break;
        }
    }

check_glue:
    /* was the caller called through glue code? Then undo the glue too. */
    if (next == 0 || next + 4 == 0 || est < lowlim || est > highlim || (est & 7))
        return next;
    if (next < 0x10000 || next >= 0x7fff0000u)
        return next;
    i = r32(next);
    if ((i >> 26) != 18)
        return next;
    {
        u32 t = BRANCH_TARGET(i, next);
        if (fe && t >= r32(fe) && t < r32(fe + 4))
            return next;
        if (t < 0x10000 || t >= 0x7fff0000u)
            return next;
        fe = ppc_lookup(t);
        if (fe) {
            i = r32(t);
            if ((i >> 26) == 32 && ((i >> 16) & 31) == 2) {
                i = r32(t + 4);
                if ((i >> 26) == 18 && (i & 1)) {
                    t = BRANCH_TARGET(i, t + 4);
                    fe = ppc_lookup(t);
                }
            }
        }
    }
    if (!(fe && r32(fe + 8) == 0 && r32(fe + 12) == 3))
        return next;
    it[0].end = r32(fe) - 4;
    it[0].inc = -4;
    it[0].intent = U_REV;
    it[0].begin = (r32(fe + 16) & ~3u) - 4;
    goto unwind_glue;
}

static u32 ppc_vunwind(u32 pc, u32 fe, u32 c, int *infunc, u32 *virt, u32 *real, u32 ptrs)
{
    u32 est = *virt, next;
    int inf = 0;

    next = ppc_vunwind_ex(pc, fe, c, &inf, &est, ptrs, 0, 0xffffffffu);
    if (fe) {
        *infunc = inf;
        *virt = *real = est;
    }
    return next;
}

static u32 ppc_handler(u32 fe) { return r32(fe + 8); }

static u32 ppc_cget(u32 c, int what)
{
    switch (what) {
    case C_SP: return r32(GR(c, 1));
    case C_NEXT: return r32(c + PX_LR) - 4;
    case C_FIR: return r32(c + PX_IAR);
    }
    return 0;
}

static void ppc_cset(u32 c, int what, u32 v)
{
    if (what == C_FIR)
        w32(c + PX_IAR, v);
    else if (what == C_RETVAL)
        w32(GR(c, 3), v);
}

/* the jump buffer: f14-f31 (doubles), r1, r2, r13-r31, cr, iar, type */
#define PJ_GPR1 0x90
#define PJ_GPR2 0x94
#define PJ_GPR13 0x98
#define PJ_CR 0xe4
#define PJ_IAR 0xe8

/* The end of an unwind. For longjmp, the saved registers come from the jump buffer.
   The TOC (r2) must be the one of the function that has the target address. */
static void ppc_unwind_fix(u32 c, u32 rec)
{
    u32 tmp = (P.r[1] - PX_SIZE - 64) & ~15u, pc, est = 0;
    int inf;

    if (rec && r32(rec) == STATUS_LONGJUMP) {
        u32 jb = r32(rec + 20);
        w32(c + PX_IAR, r32(jb + PJ_IAR));
        w32(c + PX_CR, r32(jb + PJ_CR));
        w32(GR(c, 1), r32(jb + PJ_GPR1));
        w32(GR(c, 2), r32(jb + PJ_GPR2));
        for (int i = 0; i < 18; i++)
            w64(FPR(c, 14 + i), r64(jb + 8 * (u32)i));
        for (int i = 0; i < 19; i++)
            w32(GR(c, 13 + i), r32(jb + PJ_GPR13 + 4 * (u32)i));
    }
    memcpy(G + tmp, G + c, PX_SIZE);
    pc = r32(c + PX_IAR);
    if (pc >= 0x10000 && pc < 0x7fff0000u) {
        ppc_vunwind_ex(pc, ppc_lookup(pc), tmp, &inf, &est, 0, 0xffffffffu, 0);
        w32(GR(c, 2), r32(GR(tmp, 2)));
    }
}

/* setjmp of NTDLL: the buffer gets the frame of the caller and the return address */
static u32 ppc_nt_setjmp(u32 jb)
{
    u32 lr = P.lr, tmp = (P.r[1] - PX_SIZE - 64) & ~15u, est = P.r[1];
    int inf;

    ppc_capture(tmp, lr);
    ppc_vunwind_ex(lr - 4, ppc_lookup(lr - 4), tmp, &inf, &est, 0, 0, 0xffffffffu);
    w32(jb, est);
    w32(jb + 4, lr);
    return 0;
}

static void ppc_nt_longjmp(u32 jb, u32 val)
{
    seh_unwind(UW_NORMAL, r32(jb), r32(jb + 4), 0, val ? val : 1);
}

static void ppc_state_save(void *p) { memcpy(p, &P, sizeof P); }

static void ppc_state_load(const void *p, u32 teb)
{
    (void)teb; /* it is in register 13 */
    memcpy(&P, p, sizeof P);
    ppc_round();
}

const struct cpu cpu_ppc = {
    "PowerPC", 4096, 604, 3,
    ppc_init, ppc_thunk, ppc_thunk_ptr, ppc_run, ppc_call_begin, ppc_call_end,
    ppc_pc, ppc_sp, ppc_ra, ppc_set_sp, ppc_dump,
    PX_SIZE, 7, 0, 1,
    ppc_capture, ppc_restore, ppc_lookup, ppc_vunwind, ppc_handler, ppc_cget, ppc_cset,
    ppc_unwind_fix, ppc_nt_setjmp, ppc_nt_longjmp,
    sizeof P, ppc_state_save, ppc_state_load,
    0,
};
