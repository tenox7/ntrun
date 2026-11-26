/*
 * ntrun: run Windows NT console programs made for MIPS, Alpha, PowerPC or x86
 * on a Unix host. A CPU interpreter runs the program's code; the Win32
 * functions it imports from KERNEL32 are done here, on the host.
 */
#ifndef NTRUN_H
#define NTRUN_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define M_MIPS 0x166
#define M_MIPS3K 0x162
#define M_ALPHA 0x184
#define M_PPC 0x1f0
#define M_I386 0x14c
#define M_AXP64 0x284 /* 64-bit Alpha: ntrun does not run it, it only knows the name */
#define M_IA64 0x200  /* Itanium: the same */

extern u8 *G; /* guest address 0 */
extern u64 icount;
extern int trace_insn;

static inline u8 r8(u32 a) { return G[a]; }
static inline u16 r16(u32 a) { u16 v; memcpy(&v, G + a, 2); return v; }
static inline u32 r32(u32 a) { u32 v; memcpy(&v, G + a, 4); return v; }
static inline u64 r64(u32 a) { u64 v; memcpy(&v, G + a, 8); return v; }
static inline void w8(u32 a, u8 v) { G[a] = v; }
static inline void w16(u32 a, u16 v) { memcpy(G + a, &v, 2); }
static inline void w32(u32 a, u32 v) { memcpy(G + a, &v, 4); }
static inline void w64(u32 a, u64 v) { memcpy(G + a, &v, 8); }

/* the thunks of host functions: one slot of 16 bytes for each */
#define THUNK_BASE 0x7ffd0000u
#define THUNK_SLOT 16
#define THUNK_MAX 4096
#define API_RETURN 0 /* thunk 0 ends a call made from the host */
#define IN_THUNKS(pc) ((pc) >= THUNK_BASE && (pc) < THUNK_BASE + THUNK_MAX * THUNK_SLOT)

/* Windows status codes */
#ifdef _WIN32 /* windows.h has these with other types */
#undef STATUS_BREAKPOINT
#undef STATUS_LONGJUMP
#undef STATUS_ACCESS_VIOLATION
#undef STATUS_ILLEGAL_INSTRUCTION
#undef STATUS_UNWIND
#undef STATUS_INTEGER_DIVIDE_BY_ZERO
#undef STATUS_INTEGER_OVERFLOW
#undef STATUS_PRIVILEGED_INSTRUCTION
#undef STATUS_FLOAT_DIVIDE_BY_ZERO
#undef STATUS_FLOAT_INVALID_OPERATION
#undef STATUS_FLOAT_OVERFLOW
#undef STATUS_FLOAT_UNDERFLOW
#undef STATUS_ARRAY_BOUNDS_EXCEEDED
#endif
#define STATUS_BREAKPOINT 0x80000003u
#define STATUS_LONGJUMP 0x80000026u
#define STATUS_ACCESS_VIOLATION 0xc0000005u
#define STATUS_ILLEGAL_INSTRUCTION 0xc000001du
#define STATUS_UNWIND 0xc0000027u
#define STATUS_INTEGER_DIVIDE_BY_ZERO 0xc0000094u
#define STATUS_INTEGER_OVERFLOW 0xc0000095u
#define STATUS_PRIVILEGED_INSTRUCTION 0xc0000096u
#define STATUS_FLOAT_DIVIDE_BY_ZERO 0xc000008eu
#define STATUS_FLOAT_INVALID_OPERATION 0xc0000090u
#define STATUS_FLOAT_OVERFLOW 0xc0000091u
#define STATUS_FLOAT_UNDERFLOW 0xc0000093u
#define STATUS_ARRAY_BOUNDS_EXCEEDED 0xc000008cu

/* from the core */
u32 api_dispatch(u32 index, const u32 *args); /* runs host function `index` */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
u32 find_pdata(u32 pc, u32 *size); /* the function table of the image that has pc; 0 if none */
void stack_limits(u32 *low, u32 *high);
u32 guest_call(u32 fn, int n, const u32 *args); /* call guest code from the host */
u32 guest_call2(u32 fn, int n, const u32 *args, u32 ctx, u32 cpc);
void guest_resume(void) __attribute__((noreturn)); /* go on in the call level that owns the CPU's sp */
int guest_cross(u32 sp, u32 ctx, u32 *next);
void guest_level_check(void);   /* x86: go to the call level that owns the stack pointer, if this one was left */
int api_argbytes(u32 index);    /* x86: the bytes of arguments host function `index` takes off the stack; -1: none */
u32 guest_teb(void);            /* the TEB of the thread that runs */

/* from the core, for the command processor */
#include <stddef.h>
extern char **genv; /* the environment of the program: "NAME=value" texts */
extern int ngenv;
const char *env_get(const char *name);
void env_set(const char *name, const char *value); /* value NULL: the variable goes away */
void to_host(const char *g, char *out, size_t n);  /* a Windows path as a host path */
void to_guest(const char *h, char *out, size_t n); /* a host path as a Windows path (Z:\...) */
int find_file(const char *dir, const char *name, char *out, size_t n); /* NAME in the host folder DIR, in any case */
int find_program(const char *name, char *out, size_t n); /* the .exe for a command name; 0 if none */
int wild_name(const char *pat, const char *name);        /* does NAME fit the pattern (* and ?) */
u32 run_program(const char *hp, const char *line);       /* run it in a new ntrun, give its exit code */
#ifdef _WIN32
intptr_t start_program(const char *hp, const char *line); /* the same, without the wait: the process */
u32 wait_program(intptr_t proc);                          /* wait for it, give its exit code */
#endif
void guest_exit(u32 code) __attribute__((noreturn));

/* from cmd.c: the command processor */
int cmd_is_name(const char *prog); /* is this the name of the command processor (cmd.exe, command.com) */
u32 cmd_shell(const char *cmdline);

/* from seh.c: Windows exceptions */
void seh_init(void);
u32 seh_lookup(u32 pc);
u32 seh_vunwind(u32 pc, u32 fe, u32 ctx, int *infunc, u32 *virt, u32 *real, u32 ptrs);
void seh_raise(u32 code, u32 flags, u32 nargs, u32 args) __attribute__((noreturn));
void seh_fault(u32 code, u32 pc, u32 nparams, u32 p0, u32 p1) __attribute__((noreturn));
enum { UW_NORMAL, UW_RFP, UW_RETURN };
void seh_unwind(int mode, u32 target, u32 target_ip, u32 rec, u32 retval) __attribute__((noreturn));
void seh_unwind_chain(u32 target, u32 target_ip, u32 rec, u32 retval); /* RtlUnwind of x86: it returns */

/* what a CPU reads or sets in a CONTEXT */
enum { C_SP, C_NEXT, C_FIR, C_RETVAL };

struct cpu {
    const char *name;
    u32 pagesize, proctype, procarch;
    void (*init)(u32 sp, u32 teb, u32 gp);
    void (*thunk)(u32 addr);      /* write the code of a thunk */
    u32 (*thunk_ptr)(u32 addr);   /* what an import slot holds for it */
    void (*run)(void);            /* run guest code until the return thunk */
    void (*call_begin)(u32 fn, int n, const u32 *args, u64 *save); /* set up a call from the host */
    u32 (*call_end)(const u64 *save);                              /* its result; the CPU as before */
    u32 (*pc)(void);
    u32 (*sp)(void);
    u32 (*ra)(void);              /* where the host function that runs now returns to */
    void (*set_sp)(u32 sp);
    void (*dump)(void);

    /* exceptions */
    u32 ctx_size;                 /* size of a CONTEXT */
    u32 frame_mask;               /* these bits of a frame address are 0 */
    int frame_pair;               /* RtlVirtualUnwind gives two frame pointers (Alpha) */
    int unwind_null;              /* vunwind works without a function table entry (PowerPC) */
    void (*capture)(u32 ctx, u32 fir);
    void (*restore)(u32 ctx);
    u32 (*lookup)(u32 pc);
    u32 (*vunwind)(u32 pc, u32 fe, u32 ctx, int *infunc, u32 *virt, u32 *real, u32 ptrs);
    u32 (*handler)(u32 fe);       /* the handler of this function, 0 if none */
    u32 (*cget)(u32 ctx, int what);
    void (*cset)(u32 ctx, int what, u32 v);
    void (*unwind_fix)(u32 ctx, u32 rec); /* just before an unwind restores the context; may be 0 */
    u32 (*nt_setjmp)(u32 jb);     /* setjmp and longjmp of NTDLL */
    void (*nt_longjmp)(u32 jb, u32 val);

    /* threads: the registers of the thread that does not run are kept apart */
    u32 state_size;
    void (*state_save)(void *p);
    void (*state_load)(const void *p, u32 teb); /* and the rounding mode of the host */

    int chain; /* x86: the exception handlers are in a list at the start of the TEB; no function tables */
};
extern const struct cpu cpu_mips, cpu_alpha, cpu_ppc, cpu_x86;
extern const struct cpu *cpu;

/* a CPU trap (bad instruction, break, access to bad memory): becomes a Windows exception */
void cpu_fault(u32 code, u32 pc, u32 insn) __attribute__((noreturn));

#endif
