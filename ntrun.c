/*
 * ntrun.c: the core. Guest memory, the PE loader, handles and the Win32
 * functions of KERNEL32 that the compilers of the NT SDKs call.
 */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#define _CRT_RAND_S
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "win32_compat.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "ntrun.h"

#ifdef _WIN32
/* win32_compat.h gives 4096: the limits.h of mingw has 260, too small for the host paths */
_Static_assert(PATH_MAX >= 4096, "PATH_MAX is too small for host paths");
#endif

#ifndef _WIN32
extern char **environ;
#endif

u8 *G;
u64 icount;
int trace_insn;
static int trace_api;
static const char *cur_api = "?"; /* the Windows function that runs now */
const struct cpu *cpu;
static u32 machine;
static u32 last_error;
static u32 teb;
static char self_path[PATH_MAX];
static char exe_hpath[PATH_MAX];
static char exe_hdir[PATH_MAX];
static u32 cmdline_g;

#define GSIZE 0x80000000ull
#ifdef _WIN32
#define DLLPATH_SEP ";"
#define HOST_DRIVE_PATH(h) (isalpha((unsigned char)(h)[0]) && (h)[1] == ':')
#define HOST_NULL "NUL"
#define HOST_TTY "CON"
#else
#define DLLPATH_SEP ":"
#define HOST_DRIVE_PATH(h) 0
#define HOST_NULL "/dev/null"
#define HOST_TTY "/dev/tty"
#endif
#define INVALID 0xffffffffu

void die(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "ntrun: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(125);
}

/* The end of this process, with the 32-bit exit code of Windows. The parent ntrun reads
   the full code from a pipe; the host only keeps 8 bits. */
static void exit_files(void);
static void views_sync(void);
static void views_sync_fd(int fd);
void guest_exit(u32 code) __attribute__((noreturn));
void guest_exit(u32 code)
{
    const char *s = getenv("NTRUN_EXITFD");

    exit_files();
    fflush(stdout);
    fflush(stderr);
    if (getenv("NTRUN_STATS"))
        fprintf(stderr, "ntrun: %llu instructions\n", (unsigned long long)icount);
#ifdef _WIN32
    (void)s;
    exit((int)code); /* Windows keeps all 32 bits of the code */
#else
    if (s && write(atoi(s), &code, 4) != 4)
        s = NULL;
    exit(code && !(code & 0xff) ? 1 : (int)(code & 0xff));
#endif
}

/*
 * Calls from the host into guest code nest: the program's start, the start function of a
 * DLL, an exception handler. Each has a level. When an unwind or a longjmp restores a
 * context, the host goes back to the level that owns the stack frame of that context.
 * ctx, cpc: for the call of an exception or unwind handler, the context and the address
 * where a walk up the stack goes on when it comes to the end of this level.
 */
struct level { sigjmp_buf jb; u32 sp, ctx, cpc; };
static struct level levels[64];
static int nlevels;
static volatile int in_api; /* a host function runs now, not guest code */
static u32 fault_addr, fault_write;

/*
 * Threads. Each thread of the program is a host thread, but only one of them runs at a
 * time: the one that has the CPU. It gives the CPU away when it waits (Sleep, a wait
 * function, a critical section that another thread has, a blocking read) and, now and then,
 * at a call of a Windows function. So the state of ntrun needs no locks.
 * The variables above (levels, in_api, cur_api, teb) are those of the thread with the CPU.
 */
#define NTHREADS 256
struct thread {
    int used, done, killed, suspend;
    pthread_t pt;
    u32 id, teb, start, param, exit_code;
    void *regs; /* its registers while it does not have the CPU */
    struct level lv[64];
    int nlv, in_api;
    const char *api;
    u32 tls[64];
};
static struct thread threads[NTHREADS];
static struct thread *cur = &threads[0];
static int nthreads = 1; /* threads that have not ended */
static int multi;        /* the program made a second thread */

static pthread_mutex_t cpu_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cpu_cv = PTHREAD_COND_INITIALIZER;
static unsigned long cpu_head, cpu_tail; /* the threads take the CPU in the order they ask for it */
static int cpu_busy = 1;

static void thread_finish(struct thread *t, u32 code) __attribute__((noreturn));

static void cpu_free(void)
{
    pthread_mutex_lock(&cpu_mu);
    cpu_busy = 0;
    pthread_cond_broadcast(&cpu_cv);
    pthread_mutex_unlock(&cpu_mu);
}

/* the thread that has the CPU gives it away */
static void cpu_give(void)
{
    struct thread *t = cur;

    cpu->state_save(t->regs);
    memcpy(t->lv, levels, sizeof levels[0] * (size_t)nlevels);
    t->nlv = nlevels;
    t->in_api = in_api;
    t->api = cur_api;
    cpu_free();
}

/* the thread T waits for the CPU and takes it */
static void cpu_take(struct thread *t)
{
    for (;;) {
        unsigned long my;
        pthread_mutex_lock(&cpu_mu);
        my = cpu_tail++;
        while (cpu_busy || my != cpu_head)
            pthread_cond_wait(&cpu_cv, &cpu_mu);
        cpu_busy = 1;
        cpu_head++;
        pthread_mutex_unlock(&cpu_mu);
        if (!t->suspend || t->killed)
            break;
        cpu_free(); /* SuspendThread: look again in a moment */
        usleep(2000);
    }
    cur = t;
    teb = t->teb;
    cpu->state_load(t->regs, t->teb);
    memcpy(levels, t->lv, sizeof levels[0] * (size_t)t->nlv);
    nlevels = t->nlv;
    in_api = t->in_api;
    cur_api = t->api;
    if (t->killed)
        thread_finish(t, t->exit_code);
}

/* let the other threads run */
static void thread_yield(void)
{
    struct thread *me = cur;

    if (multi) {
        cpu_give();
        cpu_take(me);
    }
}

/* wait a moment for something another thread, another process or the clock brings */
static void thread_wait(int n)
{
    struct thread *me = cur;
    int m = multi;

    if (m && n < 20) { /* at first only let the others run */
        cpu_give();
        cpu_take(me);
        return;
    }
    if (m)
        cpu_give();
    usleep(1000);
    if (m)
        cpu_take(me);
}

u32 guest_call2(u32 fn, int n, const u32 *args, u32 ctx, u32 cpc)
{
    u64 save[8];
    volatile int lv = nlevels, api0 = in_api;

    if (nlevels == 64)
        die("guest calls nest too deep");
    levels[lv].sp = cpu->sp();
    levels[lv].ctx = ctx;
    levels[lv].cpc = cpc;
    cpu->call_begin(fn, n, args, save);
    nlevels = lv + 1;
    switch (sigsetjmp(levels[lv].jb, 1)) {
    case 1: /* a context was restored into this level */
        nlevels = lv + 1;
        break;
    case 2: /* the guest code touched memory it must not */
        nlevels = lv + 1;
        in_api = 0;
        /* Windows NT for PowerPC gives the address with the "write" bit in its lowest bit */
        seh_fault(STATUS_ACCESS_VIOLATION, cpu->pc(), 2, fault_write,
            machine == M_PPC ? (fault_addr & ~1u) | fault_write : fault_addr);
    }
    in_api = 0;
    cpu->run();
    if (cpu->chain && cpu->sp() > levels[lv].sp)
        guest_resume(); /* x86: the return thunk of a level above this one */
    in_api = api0;
    nlevels = lv;
    return cpu->call_end(save);
}

u32 guest_call(u32 fn, int n, const u32 *args) { return guest_call2(fn, n, args, 0, 0); }

void guest_resume(void)
{
    u32 sp = cpu->sp();

    /* x86: a function that returns has the stack pointer where the level started, or just below */
    for (int lv = nlevels - 1; lv >= 0; lv--)
        if (cpu->chain ? levels[lv].sp >= sp : levels[lv].sp > sp)
            siglongjmp(levels[lv].jb, 1);
    die("no call level owns the stack pointer %08x", sp);
}

/* On x86 the program does an unwind itself: a handler that the host called goes to a frame
   above it and never returns. When the stack pointer is above the place where the newest call
   level started, that level was left behind: go on in the level that owns the stack pointer. */
void guest_level_check(void)
{
    if (nlevels > 0 && cpu->sp() > levels[nlevels - 1].sp)
        guest_resume();
}

u32 guest_teb(void) { return teb; }

/* A walk up the stack came to the end of a call level (the return to the host), with the
   stack pointer sp. If that level runs a handler, the walk goes on in the frame the handler
   was called for: ctx gets that frame's context, *next the address. 1 = went on */
int guest_cross(u32 sp, u32 ctx, u32 *next)
{
    u32 fir;
    int lv;

    for (lv = nlevels - 1; lv >= 0 && levels[lv].sp <= sp; lv--)
        ;
    if (lv < 0 || !levels[lv].ctx)
        return 0;
    fir = cpu->cget(ctx, C_FIR);
    memmove(G + ctx, G + levels[lv].ctx, cpu->ctx_size);
    cpu->cset(ctx, C_FIR, fir);
    *next = levels[lv].cpc;
    return 1;
}

/* a trap of the CPU: a Windows exception for the program */
void cpu_fault(u32 code, u32 pc, u32 insn)
{
    if (getenv("NTRUN_DUMP")) {
        fflush(stdout);
        fprintf(stderr, "ntrun: trap %08x at pc=%08x (instruction %08x), after %llu instructions\n", code, pc, insn,
            (unsigned long long)icount);
        cpu->dump();
    }
    seh_fault(code, pc, 0, 0, 0);
}

#ifdef _WIN32
/* The guest code touched memory it must not: go on in a function that jumps back to the
   call level. The handler sets the program counter to it, with the address of the fault as the
   return address, so that the unwinder of the C library finds a proper frame. */
static void __attribute__((noreturn, force_align_arg_pointer)) fault_jump(void)
{
    siglongjmp(levels[nlevels - 1].jb, 2);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    u8 *p;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er->ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    p = (u8 *)er->ExceptionInformation[1];
    if (p >= G && p < G + 0x100000000ull && nlevels > 0 && !in_api) {
        /* the program's own fault: its exception handlers get it */
        DWORD64 *sp;
        fault_addr = (u32)(p - G);
        fault_write = er->ExceptionInformation[0] == 1;
        sp = (DWORD64 *)(ep->ContextRecord->Rsp - 8);
        *sp = ep->ContextRecord->Rip;
        ep->ContextRecord->Rsp = (DWORD64)sp;
        ep->ContextRecord->Rip = (DWORD64)(uintptr_t)fault_jump;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    fflush(stdout);
    if (p >= G && p < G + 0x100000000ull)
        fprintf(stderr, "ntrun: access violation in the Windows function %s: guest address %08x, called from %08x\n", cur_api,
            (u32)(p - G), cpu ? cpu->pc() : 0);
    else
        fprintf(stderr, "ntrun: host fault at %p%s%s (guest pc=%08x)\n", (void *)p, in_api ? " in the Windows function " : "",
            in_api ? cur_api : "", cpu ? cpu->pc() : 0);
    if (cpu && getenv("NTRUN_DUMP"))
        cpu->dump();
    _exit(123);
}
#else
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    u8 *p = (u8 *)si->si_addr;

    (void)uc;
    if (p >= G && p < G + 0x100000000ull && nlevels > 0 && !in_api) {
        /* the program's own fault: its exception handlers get it */
        fault_addr = (u32)(p - G);
        fault_write = 0;
#if defined(__APPLE__) && defined(__aarch64__)
        fault_write = (((ucontext_t *)uc)->uc_mcontext->__es.__esr >> 6) & 1;
#elif defined(__linux__) && defined(__x86_64__)
        fault_write = (((ucontext_t *)uc)->uc_mcontext.gregs[REG_ERR] >> 1) & 1;
#elif defined(__linux__) && defined(__aarch64__)
        {
            /* the fault status is in a record of the signal frame: "ESR", its size, the value */
            const u8 *r = (const u8 *)((ucontext_t *)uc)->uc_mcontext.__reserved;
            for (int k = 0; k < 64; k++) {
                u32 magic, size;
                u64 esr;
                memcpy(&magic, r, 4);
                memcpy(&size, r + 4, 4);
                if (!magic || size < 8)
                    break;
                if (magic == 0x45535201) {
                    memcpy(&esr, r + 8, 8);
                    fault_write = (u32)(esr >> 6) & 1;
                    break;
                }
                r += size;
            }
        }
#endif
        siglongjmp(levels[nlevels - 1].jb, 2);
    }
    fflush(stdout);
    if (p >= G && p < G + 0x100000000ull)
        fprintf(stderr, "ntrun: %s in the Windows function %s: guest address %08x, called from %08x\n",
            sig == SIGBUS ? "bus error" : "access violation", cur_api, (u32)(p - G), cpu ? cpu->pc() : 0);
    else
        fprintf(stderr, "ntrun: host fault at %p%s%s (guest pc=%08x)\n", (void *)p, in_api ? " in the Windows function " : "",
            in_api ? cur_api : "", cpu ? cpu->pc() : 0);
    if (cpu && getenv("NTRUN_DUMP"))
        cpu->dump();
    _exit(123);
}
#endif

static void seterr(u32 e)
{
    last_error = e;
    if (teb)
        w32(teb + 0x34, e);
}

static u32 werr(int e)
{
    switch (e) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EMFILE: return 4;
    case EACCES: case EPERM: case EISDIR: case EROFS: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EEXIST: return 80;
    case ENOSPC: return 112;
    case ENOTEMPTY: return 145;
    case EINVAL: return 87;
    case EPIPE: return 109;
    default: return 31;
    }
}

static char *gstr(u32 a) { return a ? (char *)(G + a) : NULL; }

/* ---------------- regions of 64 KB ---------------- */

#define CHUNK 0x10000u
#define NCHUNK 0x8000u
static u8 rmap[NCHUNK];

static int region_top; /* the next region_alloc(0, ...) takes the highest free place (MEM_TOP_DOWN) */

#ifdef _WIN32
/*
 * Windows host: the guest memory is one reservation, and a region is committed when region_alloc
 * gives it to the program and decommitted (zero again) when region_free takes it back. A view of
 * a file can only be mapped at a place that is free address space, and a piece of a reservation
 * cannot be given back. So there is a zone of 32 MB (ZONE_N regions) in which each region of
 * 64 KB is a reservation of its own. region_alloc never takes it (rmap is 2 there); the views
 * of files that the program can write take their place in it (region_alloc_zone).
 */
#define ZONE_C 0x6e00u /* the first region of the zone (guest address 6e000000) */
#define ZONE_N 0x200u
#define IN_ZONE(c) ((c) >= ZONE_C && (c) < ZONE_C + ZONE_N)

static int host_reserve(void)
{
    for (int attempt = 0; attempt < 32; attempt++) {
        u8 *p = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
        u32 k;

        if (!p)
            return 0;
        VirtualFree(p, 0, MEM_RELEASE);
        if (!VirtualAlloc(p, (size_t)ZONE_C * CHUNK, MEM_RESERVE, PAGE_NOACCESS))
            continue;
        if (!VirtualAlloc(p + (size_t)(ZONE_C + ZONE_N) * CHUNK, 0x100000000ull - (size_t)(ZONE_C + ZONE_N) * CHUNK,
                MEM_RESERVE, PAGE_NOACCESS)) {
            VirtualFree(p, 0, MEM_RELEASE);
            continue;
        }
        for (k = 0; k < ZONE_N; k++)
            if (!VirtualAlloc(p + (size_t)(ZONE_C + k) * CHUNK, CHUNK, MEM_RESERVE, PAGE_NOACCESS))
                break;
        if (k == ZONE_N) {
            G = p;
            memset(rmap + ZONE_C, 2, ZONE_N);
            return 1;
        }
        /* something else took a place of the hole: give all back, try another hole */
        while (k--)
            VirtualFree(p + (size_t)(ZONE_C + k) * CHUNK, 0, MEM_RELEASE);
        VirtualFree(p, 0, MEM_RELEASE);
        VirtualFree(p + (size_t)(ZONE_C + ZONE_N) * CHUNK, 0, MEM_RELEASE);
    }
    return 0;
}

static int host_commit(u32 addr, u32 n)
{
    if (!IN_ZONE(addr / CHUNK))
        return VirtualAlloc(G + addr, (size_t)n * CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
    for (u32 k = 0; k < n; k++)
        if (!VirtualAlloc(G + addr + (size_t)k * CHUNK, CHUNK, MEM_COMMIT, PAGE_READWRITE)) {
            while (k--)
                VirtualFree(G + addr + (size_t)k * CHUNK, CHUNK, MEM_DECOMMIT);
            return 0;
        }
    return 1;
}

/* the regions have no pages, and are reserved (also those that a view of a file had) */
static void host_decommit(u32 addr, u32 n)
{
    if (!IN_ZONE(addr / CHUNK)) {
        VirtualFree(G + addr, (size_t)n * CHUNK, MEM_DECOMMIT);
        return;
    }
    for (u32 k = 0; k < n; k++) {
        u8 *p = G + addr + (size_t)k * CHUNK;
        VirtualFree(p, CHUNK, MEM_DECOMMIT);
        VirtualAlloc(p, CHUNK, MEM_RESERVE, PAGE_NOACCESS); /* fails when it is reserved still */
    }
}
#endif

static u32 region_alloc(u32 addr, u32 size)
{
    u32 n = (size + CHUNK - 1) / CHUNK, c, k;
    int top = region_top;

    region_top = 0;

    if (!n)
        n = 1;
    if (addr) {
        c = addr / CHUNK;
        if (c == 0 || c + n > NCHUNK)
            return 0;
        for (k = 0; k < n; k++)
            if (rmap[c + k])
                return 0;
    } else if (top) {
        for (c = 0x7f00 - n; c >= 0x100; c--) {
            for (k = 0; k < n && !rmap[c + k]; k++)
                ;
            if (k == n)
                break;
        }
        if (c < 0x100)
            return 0;
    } else {
        for (c = 0x100; c + n <= 0x7f00; c++) {
            for (k = 0; k < n && !rmap[c + k]; k++)
                ;
            if (k == n)
                break;
            c += k;
        }
        if (c + n > 0x7f00)
            return 0;
    }
    memset(rmap + c, 1, n);
#ifdef _WIN32
    if (!host_commit(c * CHUNK, n)) { /* the host has no memory for it: the program gets the error */
        memset(rmap + c, 0, n);
        return 0;
    }
#endif
    return c * CHUNK;
}

static void region_free(u32 addr, u32 size)
{
    u32 n = (size + CHUNK - 1) / CHUNK, c = addr / CHUNK;

    if (!n)
        n = 1;
    memset(rmap + c, 0, n);
#ifdef _WIN32
    if (IN_ZONE(c)) /* the zone stays the zone */
        memset(rmap + c, 2, n);
#endif
    /* give the host fresh zero pages there */
#ifdef _WIN32
    host_decommit(addr, n);
#else
    if (mmap(G + addr, (size_t)n * CHUNK, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == MAP_FAILED)
        die("region_free: mmap: %s", strerror(errno));
#endif
}

#ifdef _WIN32
/* a place for a view of a file: regions of the zone, committed */
static u32 region_alloc_zone(u32 size)
{
    u32 n = (size + CHUNK - 1) / CHUNK, c, k;

    if (!n)
        n = 1;
    for (c = ZONE_C; c + n <= ZONE_C + ZONE_N; c++) {
        for (k = 0; k < n && rmap[c + k] == 2; k++)
            ;
        if (k == n) {
            if (!host_commit(c * CHUNK, n))
                return 0;
            memset(rmap + c, 1, n);
            return c * CHUNK;
        }
        c += k;
    }
    return 0;
}
#endif

/* VirtualAlloc reservations */
static struct { u32 base, size; } vres[4096];
static int nvres;

/* ---------------- heap ---------------- */

#define HDR 16
#define BIG 0x80000u
#define NBINS 96
#define ARENA_BIG 0xb16b16b1u
static u32 bins[NBINS];
/* block header: +0 size|used, +4 size of the block before, +8 requested size, +12 arena tag
   free block payload: +0 next, +4 prev */

static int bin_of(u32 size)
{
    u32 u = size / 16;
    int b = 64;

    if (u < 64)
        return (int)u;
    u >>= 6;
    while (u > 1 && b < NBINS - 1) {
        u >>= 1;
        b++;
    }
    return b;
}

static void bin_add(u32 b)
{
    int i = bin_of(r32(b) & ~1u);
    u32 head = bins[i];

    w32(b + HDR, head);
    w32(b + HDR + 4, 0);
    if (head)
        w32(head + HDR + 4, b);
    bins[i] = b;
}

static void bin_del(u32 b)
{
    int i = bin_of(r32(b) & ~1u);
    u32 next = r32(b + HDR), prev = r32(b + HDR + 4);

    if (prev)
        w32(prev + HDR, next);
    else
        bins[i] = next;
    if (next)
        w32(next + HDR + 4, prev);
}

static u32 galloc(u32 req)
{
    u32 need = (req + HDR + 15) & ~15u, b, size;
    int i;

    if (need < 32)
        need = 32;
    if (need >= BIG) {
        b = region_alloc(0, need);
        if (!b)
            return 0;
        w32(b, ((need + CHUNK - 1) & ~(CHUNK - 1)) | 1);
        w32(b + 4, 0);
        w32(b + 8, req);
        w32(b + 12, ARENA_BIG);
        return b + HDR;
    }
    for (i = bin_of(need); i < NBINS; i++) {
        for (b = bins[i]; b; b = r32(b + HDR))
            if ((r32(b) & ~1u) >= need)
                goto found;
    }
    {
        /* a new arena: [block ...][end marker] */
        u32 asize = need + 64 > 0x100000 ? ((need + 64 + CHUNK - 1) & ~(CHUNK - 1)) : 0x100000;
        u32 a = region_alloc(0, asize);
        if (!a)
            return 0;
        b = a;
        w32(b, asize - HDR);
        w32(b + 4, 0);
        w32(b + 12, 0);
        w32(a + asize - HDR, 1);            /* end marker: size 0, used */
        w32(a + asize - HDR + 4, asize - HDR);
        bin_add(b);
    }
found:
    bin_del(b);
    size = r32(b) & ~1u;
    if (size - need >= 32) {
        u32 rest = b + need, after = b + size;
        w32(rest, size - need);
        w32(rest + 4, need);
        w32(rest + 12, 0);
        w32(after + 4, size - need);
        bin_add(rest);
        size = need;
    }
    w32(b, size | 1);
    w32(b + 8, req);
    return b + HDR;
}

static void gfree(u32 p)
{
    u32 b, size, next, prevsize;

    if (!p)
        return;
    b = p - HDR;
    size = r32(b) & ~1u;
    if (r32(b + 12) == ARENA_BIG) {
        region_free(b, size);
        return;
    }
    if (!(r32(b) & 1))
        return; /* double free */
    next = b + size;
    if (!(r32(next) & 1)) {
        bin_del(next);
        size += r32(next);
    }
    prevsize = r32(b + 4);
    if (prevsize && !(r32(b - prevsize) & 1)) {
        b -= prevsize;
        bin_del(b);
        size += prevsize;
    }
    w32(b, size);
    w32(b + size + 4, size);
    bin_add(b);
}

static u32 gsize(u32 p) { return r32(p - HDR + 8); }

static u32 grealloc(u32 p, u32 n, int zero, int inplace)
{
    u32 b = p - HDR, cap = (r32(b) & ~1u) - HDR, old = r32(b + 8), q;

    if (n <= cap) {
        if (zero && n > old)
            memset(G + p + old, 0, n - old);
        w32(b + 8, n);
        return p;
    }
    if (inplace)
        return 0;
    q = galloc(n);
    if (!q)
        return 0;
    memcpy(G + q, G + p, old);
    if (zero)
        memset(G + q + old, 0, n - old);
    gfree(p);
    return q;
}

u32 guest_alloc(u32 n)
{
    u32 p = galloc(n);
    if (!p)
        die("out of guest memory");
    return p;
}

static u32 gdup(const void *data, u32 n)
{
    u32 p = galloc(n ? n : 1);
    if (!p)
        die("out of guest memory");
    memcpy(G + p, data, n);
    return p;
}

/* ---------------- paths ---------------- */

/* make `path` name an entry that exists, whatever the case of its parts. 1 = it exists */
static int fix_case(char *path)
{
    struct stat st;
    char *p, *slash;

    if (stat(path, &st) == 0)
        return 1;
    p = path[0] == '/' ? path + 1 : path;
    for (;;) {
        char save;
        DIR *d;
        struct dirent *e;
        int found = 0;

        slash = strchr(p, '/');
        if (slash)
            *slash = 0;
        if (stat(path, &st) != 0) {
            /* look in the parent for the same name in another case */
            save = *p;
            if (p == path) {
                d = opendir(".");
            } else {
                p[-1] = 0;
                d = opendir(path[0] ? path : "/");
                p[-1] = '/';
            }
            (void)save;
            if (d) {
                while ((e = readdir(d)))
                    if (strcasecmp(e->d_name, p) == 0) {
                        memcpy(p, e->d_name, strlen(p));
                        found = 1;
                        break;
                    }
                closedir(d);
            }
            if (!found) {
                if (slash)
                    *slash = '/';
                return 0;
            }
        }
        if (!slash)
            return 1;
        *slash = '/';
        p = slash + 1;
        if (!*p)
            return 1;
    }
}

/*
 * Drives. A drive letter can be a folder of the host: NTRUN_DRIVE_C=/opt/nt makes C:\ that
 * folder, so C:\MSTOOLS\H is /opt/nt/MSTOOLS/H. A drive that is not set is the root of the
 * host: Z:\tmp is /tmp. Short Windows paths are good for the old tools: some keep a path
 * list in a small buffer of a fixed size.
 */
static char *drive_dir[26];    /* the host folder of a drive, no "/" at its end; NULL: the root of the host */
static int ndrives;            /* how many drives have a folder */
static char root_letter = 'Z'; /* the letter for host paths that are in no such folder */

static void drives_init(void)
{
    for (int i = 0; i < 26; i++) {
        char name[32], real[PATH_MAX];
        const char *v;
        snprintf(name, sizeof name, "NTRUN_DRIVE_%c", 'A' + i);
        v = getenv(name);
        if (!v || !*v)
            continue;
        if (!realpath(v, real))
            die("%s: cannot find the folder %s", name, v);
        {
            size_t l = strlen(real);
            if (l && real[l - 1] == '/') /* "/" is "", and "C:/" is "C:" */
                real[l - 1] = 0;
        }
        drive_dir[i] = strdup(real);
        ndrives++;
    }
    for (int i = 25; i >= 0; i--)
        if (!drive_dir[i]) {
            root_letter = (char)('A' + i);
            break;
        }
}

/* The drive that the host path H is on: the result is its letter, *rest the path below its folder. */
static int drive_of(const char *h, const char **rest)
{
    int best = -1;
    size_t bl = 0;

    for (int i = 0; i < 26; i++)
        if (drive_dir[i]) {
            size_t l = strlen(drive_dir[i]);
            if ((best < 0 || l > bl) && !strncmp(h, drive_dir[i], l) && (h[l] == '/' || !h[l])) {
                best = i;
                bl = l;
            }
        }
#ifdef _WIN32
    if (best < 0 && isalpha((unsigned char)h[0]) && h[1] == ':') {
        static char z[PATH_MAX];
        if (!ndrives) { /* C:/dir: the drive of the host */
            *rest = h + 2;
            return toupper((unsigned char)h[0]);
        }
        /* drives of folders are set: the host's drive C: is below the host root, Z:\c\dir */
        snprintf(z, sizeof z, "/%c%s", tolower((unsigned char)h[0]), h + 2);
        *rest = z;
        return root_letter;
    }
#endif
    *rest = best < 0 ? h : h + bl;
    return best < 0 ? root_letter : 'A' + best;
}

/* the host folder that is the root of a drive ("" for the root of the host);
   letter 0: the drive of the current folder */
static const char *drive_root(int letter)
{
    char cwd[PATH_MAX];
    const char *rest;

#ifdef _WIN32
    {
        static char wroot[26][3];
        if (!letter) {
            if (!getcwd(cwd, sizeof cwd))
                return "";
            letter = drive_of(cwd, &rest);
        }
        letter = toupper(letter);
        if (letter < 'A' || letter > 'Z')
            return "";
        if (drive_dir[letter - 'A'])
            return drive_dir[letter - 'A'];
        snprintf(wroot[letter - 'A'], 3, "%c:", letter); /* a drive of the host is its own folder */
        return wroot[letter - 'A'];
    }
#endif
    if (!ndrives)
        return "";
    if (!letter) {
        if (!getcwd(cwd, sizeof cwd))
            return "";
        letter = drive_of(cwd, &rest);
    }
    letter = toupper(letter);
    return letter >= 'A' && letter <= 'Z' && drive_dir[letter - 'A'] ? drive_dir[letter - 'A'] : "";
}

/* a Windows path as a host path: the drive, / for \, and the names in the case the host has */
static void to_host_plain(const char *g, char *out, size_t n)
{
    char tmp[PATH_MAX];
    const char *p = g, *root;
    int letter = 0;
    size_t len;
    char *s;

    if (!g || !*g) { /* no name: no file has it */
        out[0] = 0;
        return;
    }
    if (!strcasecmp(g, "NUL")) { snprintf(out, n, "%s", HOST_NULL); return; }
    if (!strcasecmp(g, "CON") || !strcasecmp(g, "CONOUT$") || !strcasecmp(g, "CONIN$")) { snprintf(out, n, "%s", HOST_TTY); return; }
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        letter = p[0];
        p += 2;
    }
    if (*p == '\\' || *p == '/') {
        /* from the root of the drive */
        root = drive_root(letter);
        snprintf(tmp, sizeof tmp, "%s%s", root, p);
#ifdef _WIN32
        if (ndrives) { /* Z:\c\dir is C:/dir of the host */
            char cwd[PATH_MAX];
            const char *rest;
            int l = letter;
            if (!l && getcwd(cwd, sizeof cwd))
                l = drive_of(cwd, &rest);
            if (toupper(l) == root_letter) {
                if (isalpha((unsigned char)p[1]) && (!p[2] || p[2] == 0x5c || p[2] == '/')) {
                    snprintf(tmp, sizeof tmp, "%c:%s", p[1], p[2] ? p + 2 : "/");
                } else {
                    /* Z:\name is not a drive of the host: it is below the root of the system drive
                       (Z:\windows\system32 is the system folder), not the host's Z: */
                    const char *sd = getenv("SystemDrive");
                    snprintf(tmp, sizeof tmp, "%s%s", sd && sd[0] && sd[1] == ':' ? sd : "C:", p);
                }
            }
        }
#endif
    } else if (letter && ndrives) {
        /* "C:name": below the current folder if that is on the drive, else below the drive's root */
        char cwd[PATH_MAX];
        const char *rest;
        if (getcwd(cwd, sizeof cwd) && drive_of(cwd, &rest) == toupper(letter))
            snprintf(tmp, sizeof tmp, "%s", p);
        else
            snprintf(tmp, sizeof tmp, "%s/%s", drive_root(letter), p);
    } else {
        snprintf(tmp, sizeof tmp, "%s", p);
    }
    for (s = tmp; *s; s++)
        if (*s == '\\')
            *s = '/';
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;
    if (!tmp[0])
        snprintf(tmp, sizeof tmp, ".");
    snprintf(out, n, "%s", tmp);
    fix_case(out);
}

/* the host's folder for temporary files, with no "/" at the end */
static const char *host_tmp(void)
{
    static char t[PATH_MAX];

    if (!t[0]) {
        const char *e = getenv("TMPDIR"); /* in a child process the value is a Windows path */
        size_t len;
        if (!e || !*e)
            e = "/tmp";
        if ((isalpha((unsigned char)e[0]) && e[1] == ':') || strchr(e, '\\'))
            to_host_plain(e, t, sizeof t);
        else
            snprintf(t, sizeof t, "%s", e);
        len = strlen(t);
        while (len > 1 && t[len - 1] == '/')
            t[--len] = 0;
    }
    return t;
}

void to_host(const char *g, char *out, size_t n)
{
    char tmp[PATH_MAX];

    to_host_plain(g, out, n);
    /* A name in the root folder that the host root does not have: tmpfile() and tmpnam() of the
       Microsoft C library make their files there. The host root is not a place for new files,
       so such a name is in the host's folder for temporary files. */
    if (out[0] == '/' && out[1] && !strchr(out + 1, '/')) {
        struct stat st;
        if (lstat(out, &st) < 0) {
            snprintf(tmp, sizeof tmp, "%s%s", host_tmp(), out);
            snprintf(out, n, "%s", tmp);
        }
    }
}

/* a host path as a Windows path with its drive: "C:\MSTOOLS\H", "Z:\tmp\x" */
void to_guest(const char *h, char *out, size_t n)
{
    char abs[PATH_MAX];
    const char *rest;
    int letter;
    char *s;

    if (h[0] != '/' && !(HOST_DRIVE_PATH(h))) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            snprintf(cwd, sizeof cwd, "/");
        snprintf(abs, sizeof abs, "%s/%s", strcmp(cwd, "/") ? cwd : "", h);
    } else {
        snprintf(abs, sizeof abs, "%s", h);
    }
    letter = drive_of(abs, &rest);
    snprintf(out, n, "%c:%s", letter, *rest ? rest : "/");
    for (s = out; *s; s++)
        if (*s == '/')
            *s = '\\';
}

/* the full guest path of a guest name: drive, no "." or ".." */
static void full_guest(const char *g, char *out, size_t n)
{
    char tmp[PATH_MAX * 2], cwdg[PATH_MAX], *parts[256], *s, *tok;
    int np = 0, i;
    char drive;
    const char *p = g;
    size_t o;

    to_guest(".", cwdg, sizeof cwdg); /* "C:\folder\." */
    drive = cwdg[0];
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        drive = (char)toupper((unsigned char)p[0]);
        p += 2;
    }
    if (*p == '\\' || *p == '/')
        snprintf(tmp, sizeof tmp, "%s", p);
    else if (drive == cwdg[0])
        snprintf(tmp, sizeof tmp, "%s\\%s", cwdg + 2, p); /* below the current folder */
    else
        snprintf(tmp, sizeof tmp, "\\%s", p); /* another drive: below its root */
    for (s = tmp; *s; s++)
        if (*s == '\\')
            *s = '/';
    for (tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, "."))
            continue;
        if (!strcmp(tok, "..")) {
            if (np)
                np--;
            continue;
        }
        if (np < 256)
            parts[np++] = tok;
    }
    o = (size_t)snprintf(out, n, "%c:", drive);
    if (!np)
        snprintf(out + o, n - o, "\\");
    for (i = 0; i < np && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "\\%s", parts[i]);
}

/* ---------------- environment ---------------- */

char **genv; /* the environment of the program: "NAME=value" texts */
int ngenv;

static int env_find(const char *name, size_t len)
{
    for (int i = 0; i < ngenv; i++)
        if (!strncasecmp(genv[i], name, len) && genv[i][len] == '=')
            return i;
    return -1;
}

const char *env_get(const char *name)
{
    int i = env_find(name, strlen(name));
    return i < 0 ? NULL : genv[i] + strlen(name) + 1;
}

void env_set(const char *name, const char *value)
{
    size_t len = strlen(name);
    int i = env_find(name, len);
    char *s = NULL;

    if (value) {
        s = malloc(len + strlen(value) + 2);
        sprintf(s, "%s=%s", name, value);
    }
    if (i >= 0) {
        free(genv[i]);
        if (s)
            genv[i] = s;
        else
            genv[i] = genv[--ngenv];
    } else if (s) {
        genv = realloc(genv, sizeof *genv * (size_t)(ngenv + 2));
        genv[ngenv++] = s;
    }
}

static u32 env_block(void)
{
    size_t n = 1;
    u32 p, o;

    for (int i = 0; i < ngenv; i++)
        n += strlen(genv[i]) + 1;
    p = galloc((u32)n + 1);
    o = p;
    for (int i = 0; i < ngenv; i++) {
        size_t l = strlen(genv[i]) + 1;
        memcpy(G + o, genv[i], l);
        o += (u32)l;
    }
    w8(o, 0);
    if (!ngenv)
        w8(o + 1, 0);
    return p;
}

/* ---------------- handles ---------------- */

enum { HT_NONE, HT_FILE, HT_PROC, HT_THREAD, HT_FIND, HT_MAP, HT_SYNC };
struct hnd {
    int type, fd, delete_on_close, status, done;
    int thr;           /* HT_THREAD: the number of the thread, -1: a thread of another process */
    int kind, manual;  /* HT_SYNC: what it is; an event that stays set */
    u32 owner;         /* a mutex: the thread that has it */
    s32 max;           /* a semaphore: its highest count */
    pid_t pid;
    intptr_t proc; /* Windows host: the handle of the process that a child ntrun runs in */
    char *path;
    u32 prot, size;
    s32 count;
    char **names;
    int n, pos;
    char *dir;
};
#define NHND 1024
#define HBASE 0x100u
static struct hnd H[NHND];
static u32 std_h[3];

static struct hnd *hget(u32 h)
{
    u32 i;

    if (h < HBASE || (h & 3))
        return NULL;
    i = (h - HBASE) / 4;
    if (i >= NHND || H[i].type == HT_NONE)
        return NULL;
    return &H[i];
}

static u32 hnew_at(u32 h, int type)
{
    u32 i = (h - HBASE) / 4;

    memset(&H[i], 0, sizeof H[i]);
    H[i].type = type;
    H[i].fd = -1;
    return h;
}

static u32 hnew(int type)
{
    for (u32 i = 0; i < NHND; i++)
        if (H[i].type == HT_NONE)
            return hnew_at(HBASE + i * 4, type);
    die("out of handles");
}

static u32 hfile(int fd)
{
    u32 h = hnew(HT_FILE);
    hget(h)->fd = fd;
    return h;
}

static int hfd(u32 h)
{
    struct hnd *x = hget(h);
    return x && x->type == HT_FILE ? x->fd : -1;
}

/* ---------------- modules ---------------- */

struct module {
    char name[64];
    char hpath[PATH_MAX];
    u32 base, size, entry, exp_rva, exp_size;
    int builtin, is_dll, inited, tls_done;
};
static struct module mods[64];
static int nmods;
static struct module *started[64]; /* the DLLs with a start function, in the order of their start */
static int nstarted;

struct api {
    const char *name;
    u32 (*fn)(u32 *a);
    int argb; /* x86: the bytes of its arguments + 2, when known (see api_argbytes) */
};
static struct api apis[THUNK_MAX];
static int napis;

static u32 api_thunk(const char *name)
{
    int i;

    for (i = 1; i < napis; i++)
        if (!strcmp(apis[i].name, name))
            break;
    if (i == napis) {
        if (napis == THUNK_MAX - 2)
            die("too many imports");
        apis[napis].name = strdup(name);
        apis[napis].fn = NULL;
        napis++;
    }
    cpu->thunk(THUNK_BASE + (u32)i * THUNK_SLOT);
    return cpu->thunk_ptr(THUNK_BASE + (u32)i * THUNK_SLOT);
}

static struct module *get_module(const char *name);

static int api_have(const char *name) /* does ntrun have this Windows function? */
{
    for (int i = 1; i < napis; i++)
        if (apis[i].fn && !strcmp(apis[i].name, name))
            return 1;
    return 0;
}

static u32 resolve_export(struct module *m, const char *name, u32 ord)
{
    u32 ed, nfun, nname, funcs, names, ords, base, rva = 0;

    if (m->builtin) {
        char tmp[32];
        if (!name) {
            snprintf(tmp, sizeof tmp, "%s#%u", m->name, ord);
            name = tmp;
        } else if (!strcmp(m->name, "NTDLL.DLL") && !api_have(name)) {
            /* NTDLL has C library functions too (strlen, _strlwr, ...): CRTDLL.DLL has the same ones */
            struct module *c = get_module("CRTDLL.DLL");
            u32 addr = c && !c->builtin ? resolve_export(c, name, 0) : 0;
            if (addr)
                return addr;
        }
        return api_thunk(name);
    }
    if (!m->exp_rva)
        return 0;
    ed = m->base + m->exp_rva;
    base = r32(ed + 16);
    nfun = r32(ed + 20);
    nname = r32(ed + 24);
    funcs = m->base + r32(ed + 28);
    names = m->base + r32(ed + 32);
    ords = m->base + r32(ed + 36);
    if (name) {
        u32 lo = 0, hi = nname;
        while (lo < hi) { /* the names are sorted */
            u32 mid = (lo + hi) / 2;
            int c = strcmp(name, gstr(m->base + r32(names + mid * 4)));
            if (c == 0) {
                rva = r32(funcs + r16(ords + mid * 2) * 4);
                break;
            }
            if (c < 0)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (!rva) /* not sorted after all */
            for (u32 k = 0; k < nname; k++)
                if (!strcmp(name, gstr(m->base + r32(names + k * 4)))) {
                    rva = r32(funcs + r16(ords + k * 2) * 4);
                    break;
                }
    } else if (ord - base < nfun) {
        rva = r32(funcs + (ord - base) * 4);
    }
    if (!rva)
        return 0;
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {
        /* a forwarder: "DLL.Function" */
        char fwd[256], dll[160], *dot;
        struct module *t;
        snprintf(fwd, sizeof fwd, "%s", gstr(m->base + rva));
        dot = strchr(fwd, '.');
        if (!dot)
            return 0;
        *dot = 0;
        snprintf(dll, sizeof dll, "%s.DLL", fwd);
        t = get_module(dll);
        if (!t)
            return 0;
        if (dot[1] == '#') /* "DLL.#12": by number */
            return resolve_export(t, NULL, (u32)atoi(dot + 2));
        return resolve_export(t, dot + 1, 0);
    }
    return m->base + rva;
}

int find_file(const char *dir, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, name);
    return fix_case(out);
}

/* find a guest DLL file: next to the program, NTRUN_DLLPATH (host dirs, ':'), the guest PATH */
static int find_dll(const char *name, char *out, size_t n)
{
    const char *p;
    char dir[PATH_MAX], gdir[PATH_MAX];

    if (find_file(exe_hdir, name, out, n))
        return 1;
    if ((p = getenv("NTRUN_DLLPATH"))) {
        while (*p) {
#ifdef _WIN32
            size_t l = strcspn(p, ";"); /* a drive letter has the other one */
#else
            size_t l = strcspn(p, ":");
#endif
            snprintf(dir, sizeof dir, "%.*s", (int)l, p);
            if (find_file(dir, name, out, n))
                return 1;
            p += l;
            if (*p)
                p++;
        }
    }
    if ((p = env_get("PATH"))) {
        while (*p) {
            size_t l = strcspn(p, ";");
            snprintf(gdir, sizeof gdir, "%.*s", (int)l, p);
            if (gdir[0]) {
                to_host(gdir, dir, sizeof dir);
                if (find_file(dir, name, out, n))
                    return 1;
            }
            p += l;
            if (*p)
                p++;
        }
    }
    return 0;
}

/* 2: a part of Windows that ntrun has itself. 1: ntrun has it only when no file of that
   name is found (then its functions stop the program when it calls them). */
static int is_builtin(const char *name)
{
    static const char *own[] = { "KERNEL32.DLL", "NTDLL.DLL", "USER32.DLL", "ADVAPI32.DLL", "GDI32.DLL", "RPCRT4.DLL", NULL };
    static const char *b[] = { "VERSION.DLL", "LZ32.DLL", "SHELL32.DLL", "OLE32.DLL", "OLEAUT32.DLL", "IMAGEHLP.DLL",
        "COMDLG32.DLL", "COMCTL32.DLL", "WINSPOOL.DRV", "MPR.DLL", "WSOCK32.DLL", "MGMTAPI.DLL", NULL };
    for (int i = 0; own[i]; i++)
        if (!strcmp(own[i], name))
            return 2;
    for (int i = 0; b[i]; i++)
        if (!strcmp(b[i], name))
            return 1;
    return 0;
}

static struct module *load_image(const char *hpath, const char *name);

static struct module *get_module(const char *name0)
{
    char name[64], hpath[PATH_MAX];
    struct module *m;
    int i;

    snprintf(name, sizeof name, "%s", name0);
    for (char *s = name; *s; s++)
        *s = (char)toupper((unsigned char)*s);
    if (!strchr(name, '.'))
        strcat(name, ".DLL");
    for (i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, name))
            return &mods[i];
    if (is_builtin(name) == 2 || (is_builtin(name) == 1 && !find_dll(name, hpath, sizeof hpath))) {
        if (nmods == 64)
            die("too many modules");
        m = &mods[nmods];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", name);
        m->builtin = 1;
        m->inited = 1;
        m->base = 0x7ff00000 + (u32)nmods * 0x1000; /* only a handle value */
        nmods++;
        return m;
    }
    if (!find_dll(name, hpath, sizeof hpath))
        return NULL;
    return load_image(hpath, name);
}

static void relocate(u32 base, u32 rva, u32 size, u32 delta)
{
    u32 p = base + rva, end = p + size;

    while (p + 8 <= end) {
        u32 page = r32(p), bsize = r32(p + 4), n, k;
        if (!bsize)
            break;
        n = (bsize - 8) / 2;
        for (k = 0; k < n; k++) {
            u16 e = r16(p + 8 + k * 2);
            u32 a = base + page + (e & 0xfff);
            switch (e >> 12) {
            case 0: break;
            case 1: w16(a, (u16)(r16(a) + (delta >> 16))); break;
            case 2: w16(a, (u16)(r16(a) + delta)); break;
            case 3: w32(a, r32(a) + delta); break;
            case 4: {
                u32 v = ((u32)r16(a) << 16) + (u32)(s32)(s16)r16(p + 8 + (k + 1) * 2) + delta + 0x8000;
                w16(a, (u16)(v >> 16));
                k++;
                break;
            }
            case 5: {
                u32 insn = r32(a), t = ((insn & 0x3ffffff) << 2) + delta;
                w32(a, (insn & ~0x3ffffffu) | ((t >> 2) & 0x3ffffff));
                break;
            }
            default: die("relocation type %u is not supported", e >> 12);
            }
        }
        p += bsize;
    }
}

static struct module *load_image(const char *hpath, const char *name)
{
    struct module *m;
    struct stat st;
    u8 *f;
    int fd, i;
    u32 pe, nsec, optsz, opt, sec, imgbase, imgsize, hdrsize, base, delta, mach, chars, ndir;
    u32 imp_rva, reloc_rva, reloc_size;

    fd = open(hpath, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0)
        die("cannot open %s: %s", hpath, strerror(errno));
    f = malloc((size_t)st.st_size + 1);
    if (read(fd, f, (size_t)st.st_size) != st.st_size)
        die("cannot read %s", hpath);
    close(fd);
#define F16(o) (*(u16 *)(f + (o)))
#define F32(o) (*(u32 *)(f + (o)))
    if (st.st_size < 0x100 || f[0] != 'M' || f[1] != 'Z')
        die("%s is not a Windows program", hpath);
    pe = F32(0x3c);
    if (pe + 0x100 > (u32)st.st_size || F32(pe) != 0x4550)
        die("%s is not a 32-bit Windows (PE) program", hpath);
    mach = F16(pe + 4);
    if (mach == M_MIPS3K)
        mach = M_MIPS;
    if (!machine) {
        machine = mach;
        switch (mach) {
        case M_MIPS: cpu = &cpu_mips; break;
        case M_ALPHA: cpu = &cpu_alpha; break;
        case M_PPC: cpu = &cpu_ppc; break;
        case M_I386: cpu = &cpu_x86; break;
        case M_AXP64:
            /* The tools for AXP64 are 32-bit Alpha programs, and they run. What they make
               needs a 64-bit Windows, and ntrun has the 32-bit system functions only. */
            die("%s is a program for 64-bit Alpha (AXP64): ntrun runs 32-bit programs only", hpath);
        case M_IA64: /* the same: the tools for Itanium are x86 programs */
            die("%s is a program for Itanium (IA-64): ntrun runs 32-bit programs only", hpath);
        default: die("%s: machine type 0x%x is not supported", hpath, mach);
        }
        cpu->thunk(THUNK_BASE);
    } else if (mach != machine) {
        die("%s is for another CPU (machine type 0x%x)", hpath, mach);
    }
    nsec = F16(pe + 6);
    optsz = F16(pe + 20);
    chars = F16(pe + 22);
    opt = pe + 24;
    sec = opt + optsz;
    imgbase = F32(opt + 28);
    imgsize = F32(opt + 56);
    hdrsize = F32(opt + 60);
    ndir = F32(opt + 92);

    base = region_alloc(imgbase, imgsize);
    if (!base) {
        if (chars & 1)
            die("%s cannot load at %08x and has no relocations", hpath, imgbase);
        base = region_alloc(0, imgsize);
        if (!base)
            die("no room for %s", hpath);
    }
    delta = base - imgbase;
    memcpy(G + base, f, hdrsize < (u32)st.st_size ? hdrsize : (u32)st.st_size);
    for (u32 s = 0; s < nsec; s++) {
        u32 o = sec + s * 40, vsize = F32(o + 8), va = F32(o + 12), rsize = F32(o + 16), rptr = F32(o + 20);
        u32 n = rsize;
        if (vsize && n > vsize)
            n = vsize;
        if (!rptr || !n)
            continue;
        if (va + n > ((imgsize + CHUNK - 1) & ~(CHUNK - 1)) || rptr + n > (u32)st.st_size)
            die("%s: bad section", hpath);
        memcpy(G + base + va, f + rptr, n);
    }

    if (nmods == 64)
        die("too many modules");
    m = &mods[nmods++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->hpath, sizeof m->hpath, "%s", hpath);
    m->base = base;
    m->size = imgsize;
    m->is_dll = (chars & 0x2000) != 0;
    m->entry = F32(opt + 16) ? base + F32(opt + 16) : 0;
    if (ndir > 0) {
        m->exp_rva = F32(opt + 96);
        m->exp_size = F32(opt + 100);
    }
    imp_rva = ndir > 1 ? F32(opt + 96 + 8) : 0;
    reloc_rva = ndir > 5 ? F32(opt + 96 + 40) : 0;
    reloc_size = ndir > 5 ? F32(opt + 96 + 44) : 0;
    free(f);

    if (delta && reloc_rva)
        relocate(base, reloc_rva, reloc_size, delta);

    if (imp_rva) {
        u32 d = base + imp_rva;
        for (; r32(d + 12); d += 20) {
            char dll[64];
            struct module *t;
            u32 ilt = r32(d) ? r32(d) : r32(d + 16), iat = r32(d + 16);

            snprintf(dll, sizeof dll, "%s", gstr(base + r32(d + 12)));
            i = (int)(m - mods);
            t = get_module(dll);
            m = &mods[i];
            if (!t)
                die("%s needs %s, which was not found (see NTRUN_DLLPATH)", m->name, dll);
            for (u32 k = 0;; k++) {
                u32 v = r32(base + ilt + k * 4), addr;
                if (!v)
                    break;
                if (v & 0x80000000u)
                    addr = resolve_export(t, NULL, v & 0xffff);
                else
                    addr = resolve_export(t, gstr(base + v + 2), 0);
                if (!addr)
                    die("%s: %s has no %s", m->name, t->name,
                        (v & 0x80000000u) ? "such ordinal" : gstr(base + v + 2));
                w32(base + iat + k * 4, addr);
            }
        }
    }
    return m;
}

u32 find_pdata(u32 pc, u32 *size)
{
    for (int i = 0; i < nmods; i++) {
        struct module *m = &mods[i];
        if (!m->builtin && pc >= m->base && pc < m->base + m->size) {
            u32 opt = m->base + r32(m->base + 0x3c) + 24;
            if (r32(opt + 92) <= 3 || !r32(opt + 96 + 24))
                return 0;
            *size = r32(opt + 96 + 28);
            return m->base + r32(opt + 96 + 24);
        }
    }
    return 0;
}

void stack_limits(u32 *low, u32 *high)
{
    *low = r32(teb + 8);
    *high = r32(teb + 4);
}

/* Static thread-local storage (__declspec(thread) variables): a module with a TLS directory gets
   a number and a data block; the TEB (offset 0x2c) points to the list of the blocks. There is one thread. */
#define TLS_ARRAY 0x1000 /* offset of the list in the TEB chunk */
#define TLS_MODULES 64
static u32 tls_modules;
static struct { u32 start, len, zero; } tls_data[TLS_MODULES]; /* the first data of each module */

/* the blocks of thread-local data for a new thread with the TEB at T */
static void tls_thread(u32 t)
{
    for (u32 k = 0; k < tls_modules; k++) {
        u32 blk = galloc(tls_data[k].len + tls_data[k].zero + 8);
        memcpy(G + blk, G + tls_data[k].start, tls_data[k].len);
        memset(G + blk + tls_data[k].len, 0, tls_data[k].zero + 8);
        w32(t + TLS_ARRAY + 4 * k, blk);
    }
    if (tls_modules)
        w32(t + 0x2c, t + TLS_ARRAY);
}

static void tls_setup(struct module *m)
{
    u32 opt, d, start, end, zero, blk, cb;

    if (m->tls_done)
        return;
    m->tls_done = 1;
    if (m->builtin)
        return;
    opt = m->base + r32(m->base + 0x3c) + 24;
    if (r32(opt + 92) <= 9 || !r32(opt + 96 + 72))
        return;
    d = m->base + r32(opt + 96 + 72); /* start and end of the first data, address of the number, callbacks, zero bytes */
    start = r32(d);
    end = r32(d + 4);
    zero = r32(d + 16);
    if (end < start || tls_modules == TLS_MODULES)
        die("%s: bad thread-local storage data", m->name);
    tls_data[tls_modules].start = start;
    tls_data[tls_modules].len = end - start;
    tls_data[tls_modules].zero = zero;
    blk = galloc(end - start + zero + 8);
    memcpy(G + blk, G + start, end - start);
    memset(G + blk + (end - start), 0, zero + 8);
    w32(teb + TLS_ARRAY + 4 * tls_modules, blk);
    w32(teb + 0x2c, teb + TLS_ARRAY);
    w32(r32(d + 8), tls_modules++);
    for (cb = r32(d + 12); cb && r32(cb); cb += 4) {
        u32 a[3] = { m->base, 1, 0 };
        guest_call(r32(cb), 3, a);
    }
}

/* Run the entry point of a DLL, after those of the DLLs that it imports from: the order of
   Windows. A DLL that uses the C runtime DLL in its entry point needs that one started. */
static void init_module(struct module *m)
{
    u32 opt;

    if (m->inited)
        return;
    m->inited = 1;
    opt = m->base + r32(m->base + 0x3c) + 24;
    if (r32(opt + 92) > 1 && r32(opt + 96 + 8)) {
        for (u32 d = m->base + r32(opt + 96 + 8); r32(d + 12); d += 20) {
            int i = (int)(m - mods);
            struct module *t = get_module(gstr(m->base + r32(d + 12)));
            m = &mods[i];
            if (t)
                init_module(t);
        }
    }
    if (m->is_dll && m->entry) {
        u32 a[3] = { m->base, 1, 0 };
        if (!guest_call(m->entry, 3, a))
            die("%s: its start function failed", m->name);
        started[nstarted++] = m;
    }
}

/* run the entry points of the DLLs that are loaded and not started yet */
static void init_dlls(void)
{
    for (int i = 0; i < nmods; i++)
        tls_setup(&mods[i]);
    for (int i = 0; i < nmods; i++)
        init_module(&mods[i]);
}

/* ---------------- time ---------------- */

static u64 unix_to_ft(time_t t, long nsec) { return ((u64)t + 11644473600ull) * 10000000ull + (u64)nsec / 100; }
static time_t ft_to_unix(u64 ft) { return (time_t)(ft / 10000000ull - 11644473600ull); }

static void put_systemtime(u32 p, const struct tm *tm, int ms)
{
    w16(p, (u16)(tm->tm_year + 1900));
    w16(p + 2, (u16)(tm->tm_mon + 1));
    w16(p + 4, (u16)tm->tm_wday);
    w16(p + 6, (u16)tm->tm_mday);
    w16(p + 8, (u16)tm->tm_hour);
    w16(p + 10, (u16)tm->tm_min);
    w16(p + 12, (u16)tm->tm_sec);
    w16(p + 14, (u16)ms);
}

/* ---------------- the Win32 functions ---------------- */

#define API(n) static u32 k_##n(u32 *a)
#define UNUSED (void)a

API(GetVersion) { UNUSED; return 0x05650004; } /* Windows NT 4.0, build 1381 */
API(GetLastError) { UNUSED; return last_error; }
API(SetLastError) { seterr(a[0]); return 0; }
API(GetCurrentProcess) { UNUSED; return 0xffffffff; }
API(GetCurrentThread) { UNUSED; return 0xfffffffe; }
API(GetCurrentProcessId) { UNUSED; return (u32)getpid(); }
API(GetCurrentThreadId) { UNUSED; return cur->id; }
API(GetTickCount) { UNUSED; struct timeval tv; gettimeofday(&tv, NULL); return (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000); }
API(Sleep) /* the other threads run while this one sleeps */
{
    struct thread *me = cur;
    int m = multi;

    if (m)
        cpu_give();
    if (a[0]) {
        struct timespec ts = { a[0] / 1000, (long)(a[0] % 1000) * 1000000 };
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
    }
    if (m)
        cpu_take(me);
    return 0;
}
API(Beep) { UNUSED; return 1; }
API(SetErrorMode) { UNUSED; return 0; }
API(SetHandleCount) { return a[0]; }
API(SetConsoleCtrlHandler) { UNUSED; return 1; }
static u32 top_filter; /* the function the program gave to SetUnhandledExceptionFilter */
API(SetUnhandledExceptionFilter)
{
    u32 old = top_filter;
    top_filter = a[0];
    return old;
}
API(DisableThreadLibraryCalls) { UNUSED; return 1; }
API(GetACP) { UNUSED; return 1252; }
API(GetOEMCP) { UNUSED; return 437; }
API(IsValidCodePage) { return a[0] == 1252 || a[0] == 437; }
API(IsValidLocale) { UNUSED; return 1; }
API(GetUserDefaultLCID) { UNUSED; return 0x409; }
/* Is the memory from ADDR for LEN bytes the program's? Everywhere else the host gives each address from
   0x10000 to 2 GB. On a Windows host only the regions that the program got are there: the rest faults. */
static int guest_ok(u32 addr, u32 len)
{
    u64 end = (u64)addr + len;

    if (addr < 0x10000 || end > GSIZE)
        return 0;
#ifdef _WIN32
    for (u32 c = addr / CHUNK; c <= (u32)((end - (len ? 1 : 0)) / CHUNK); c++)
        if (c >= NCHUNK || rmap[c] != 1)
            return 0;
#endif
    return 1;
}

API(IsBadReadPtr) { return !guest_ok(a[0], a[1]); }
API(IsBadWritePtr) { return !guest_ok(a[0], a[1]); }
API(IsBadCodePtr) { return !guest_ok(a[0], 1); }

/* ExitProcess tells each DLL that the program ends (reason 0), the DLL that started last first.
   The C runtime DLL of 2005 writes the rest of the output of the program at that time. */
API(ExitProcess)
{
    static int ending;
    if (!ending) {
        ending = 1;
        for (int i = nstarted - 1; i >= 0; i--) {
            u32 b[3] = { started[i]->base, 0, 1 };
            guest_call(started[i]->entry, 3, b);
        }
    }
    guest_exit(a[0]);
}
API(TerminateProcess) { guest_exit(a[1]); }
API(DebugBreak) { UNUSED; seh_fault(STATUS_BREAKPOINT, cpu->ra() - 4, 0, 0, 0); }

/* exceptions: see seh.c */
API(RaiseException) { seh_raise(a[0], a[1], a[2], a[3]); }
API(RtlRaiseException)
{
    u32 rec = a[0], n = r32(rec + 16);
    seh_raise(r32(rec), r32(rec + 4), n, rec + 20);
}
API(RtlUnwind)
{
    if (cpu->chain) { /* x86: it comes back, the program goes to its place itself */
        seh_unwind_chain(a[0], a[1], a[2], a[3]);
        return a[3];
    }
    seh_unwind(UW_NORMAL, a[0], a[1], a[2], a[3]);
}
API(RtlUnwindRfp) { seh_unwind(UW_RFP, a[0], a[1], a[2], a[3]); }
API(RtlUnwindReturn) { seh_unwind(UW_RETURN, a[0], 0, a[1], a[2]); }
API(RtlLookupFunctionEntry) { return seh_lookup(a[0]); }
API(RtlCaptureContext) { cpu->capture(a[0], cpu->ra()); return 0; }
API(setjmp) { return cpu->nt_setjmp(a[0]); }
API(longjmp) { cpu->nt_longjmp(a[0], a[1]); return 0; }

API(RtlVirtualUnwind)
{
    int infunc = 0;
    u32 virt = 0, real = 0, next;

    if (a[4]) { /* PowerPC leaves the frame as it is when there is no function entry */
        virt = r32(a[4]);
        real = cpu->frame_pair ? r32(a[4] + 4) : virt;
    }
    if (!a[1] && !cpu->unwind_null)
        return cpu->cget(a[2], C_NEXT);
    next = seh_vunwind(a[0], a[1], a[2], &infunc, &virt, &real, a[5]);
    if (a[3] && a[1])
        w8(a[3], (u8)infunc);
    if (a[4]) {
        w32(a[4], virt);
        if (cpu->frame_pair)
            w32(a[4] + 4, real);
    }
    return next;
}

/* The C run-time calls this from its handler around main(). It is the end of the program:
   say what the exception was, and let the run-time exit with the code. */
API(UnhandledExceptionFilter)
{
    u32 rec = a[0] ? r32(a[0]) : 0;

    if (top_filter) { /* the filter of the program: 1 it handled it, 0 it did not, -1 go on */
        u32 r = guest_call(top_filter, 1, &a[0]);
        if (r)
            return r;
    }
    fflush(stdout);
    if (rec) {
        fprintf(stderr, "ntrun: unhandled exception %08x in the program at address %08x", r32(rec), r32(rec + 12));
        if (r32(rec) == STATUS_ACCESS_VIOLATION && r32(rec + 16) >= 2)
            fprintf(stderr, ": it could not %s address %08x", r32(rec + 20) ? "write to" : "read", r32(rec + 24));
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "ntrun: unhandled exception in the program\n");
    }
    return 1; /* EXCEPTION_EXECUTE_HANDLER */
}

API(GetCPInfo)
{
    memset(G + a[1], 0, 20);
    w32(a[1], 1);
    w8(a[1] + 4, '?');
    return 1;
}

API(GetSystemInfo)
{
    u32 p = a[0];
    memset(G + p, 0, 36);
    w16(p, (u16)cpu->procarch);
    w32(p + 4, cpu->pagesize);
    w32(p + 8, 0x10000);
    w32(p + 12, 0x7ffeffff);
    w32(p + 16, 1);
    w32(p + 20, 1);
    w32(p + 24, cpu->proctype);
    w32(p + 28, 0x10000);
    w16(p + 32, 4);
    return 0;
}

API(GetCommandLineA) { UNUSED; return cmdline_g; }
API(GetEnvironmentStrings) { UNUSED; return env_block(); }
API(GetEnvironmentStringsW) { UNUSED; seterr(120); return 0; }
API(FreeEnvironmentStringsA) { gfree(a[0]); return 1; }
API(FreeEnvironmentStringsW) { UNUSED; return 1; }

API(SetEnvironmentVariableA)
{
    env_set(gstr(a[0]), gstr(a[1]));
    return 1;
}

static u32 res2_g, res2_len;

API(GetStartupInfoA)
{
    u32 p = a[0];
    memset(G + p, 0, 68);
    w32(p, 68);
    w32(p + 44, 0x100);
    w16(p + 50, (u16)res2_len);
    w32(p + 52, res2_g);
    w32(p + 56, std_h[0]);
    w32(p + 60, std_h[1]);
    w32(p + 64, std_h[2]);
    return 0;
}

API(GetStdHandle)
{
    switch ((s32)a[0]) {
    case -10: return std_h[0];
    case -11: return std_h[1];
    case -12: return std_h[2];
    }
    seterr(6);
    return INVALID;
}

API(SetStdHandle)
{
    switch ((s32)a[0]) {
    case -10: std_h[0] = a[1]; return 1;
    case -11: std_h[1] = a[1]; return 1;
    case -12: std_h[2] = a[1]; return 1;
    }
    seterr(6);
    return 0;
}

API(GetFileType)
{
    struct stat st;
    int fd = hfd(a[0]);

    if (fd < 0 || fstat(fd, &st) < 0) {
        seterr(6);
        return 0;
    }
    if (S_ISCHR(st.st_mode))
        return 2;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return 3;
    return 1;
}

/* heaps: every heap is the one allocator */
API(HeapCreate) { UNUSED; return galloc(64); }
API(HeapDestroy) { UNUSED; return 1; }
API(HeapAlloc)
{
    u32 p = galloc(a[2]);
    if (p && (a[1] & 8))
        memset(G + p, 0, a[2]);
    if (!p)
        seterr(8);
    return p;
}
API(HeapFree) { gfree(a[2]); return 1; }
API(HeapReAlloc)
{
    u32 p = a[2] ? grealloc(a[2], a[3], (a[1] & 8) != 0, (a[1] & 0x10) != 0) : 0;
    if (!p)
        seterr(8);
    return p;
}
API(HeapSize) { return gsize(a[2]); }
API(HeapValidate) { UNUSED; return 1; }
API(HeapCompact) { UNUSED; return 0; }
API(HeapLock) { UNUSED; return 1; }
API(HeapUnlock) { UNUSED; return 1; }
API(HeapWalk) { UNUSED; seterr(259); return 0; }

API(VirtualAlloc)
{
    u32 addr = a[0], size = a[1], type = a[2];

    if (type & 0x2000) { /* MEM_RESERVE */
        u32 base;
        region_top = !addr && (type & 0x100000); /* MEM_TOP_DOWN */
        base = region_alloc(addr & ~(CHUNK - 1), size + (addr & (CHUNK - 1)));
        if (!base) {
            seterr(addr ? 487 : 8);
            return 0;
        }
        if (nvres == 4096)
            die("too many VirtualAlloc regions");
        vres[nvres].base = base;
        vres[nvres].size = size + (addr & (CHUNK - 1));
        nvres++;
        return base;
    }
    if (type & 0x1000) { /* MEM_COMMIT */
        if (!addr) {
            u32 b[4] = { 0, size, 0x3000, a[3] };
            return k_VirtualAlloc(b);
        }
        for (int i = 0; i < nvres; i++)
            if (addr >= vres[i].base && addr < vres[i].base + ((vres[i].size + CHUNK - 1) & ~(CHUNK - 1)))
                return addr & ~(cpu->pagesize - 1);
        {
            /* a commit that also reserves */
            u32 b[4] = { addr, size, 0x3000, a[3] };
            return k_VirtualAlloc(b);
        }
    }
    seterr(87);
    return 0;
}

API(VirtualFree)
{
    u32 addr = a[0], size = a[1], type = a[2];

    if (type & 0x8000) { /* MEM_RELEASE */
        for (int i = 0; i < nvres; i++)
            if (vres[i].base == addr) {
                region_free(addr, vres[i].size);
                vres[i] = vres[--nvres];
                return 1;
            }
        seterr(487);
        return 0;
    }
    if (type & 0x4000) { /* MEM_DECOMMIT: the pages read as zero the next time */
        u32 ps = cpu->pagesize, s = addr & ~(ps - 1), e = (addr + size + ps - 1) & ~(ps - 1);
        if (!size) {
            for (int i = 0; i < nvres; i++)
                if (vres[i].base == addr)
                    e = addr + vres[i].size;
        }
        if (e > s)
            memset(G + s, 0, e - s);
        return 1;
    }
    seterr(87);
    return 0;
}

API(VirtualProtect)
{
    if (a[3])
        w32(a[3], 4);
    return 1;
}

/* files */
API(CreateFileA)
{
    char hp[PATH_MAX];
    u32 access = a[1], disp = a[4], flags = a[5], h;
    int oflag, fd;
    struct stat st;

    to_host(gstr(a[0]), hp, sizeof hp);
    if ((access & 0xc0000000u) == 0xc0000000u || (access & 0x10000000u))
        oflag = O_RDWR;
    else if (access & 0x40000000u)
        oflag = O_WRONLY;
    else
        oflag = O_RDONLY;
    switch (disp) {
    case 1: oflag |= O_CREAT | O_EXCL; break;
    case 2: oflag |= O_CREAT | O_TRUNC; break;
    case 3: break;
    case 4: oflag |= O_CREAT; break;
    case 5: oflag |= O_TRUNC; break;
    default: seterr(87); return INVALID;
    }
    if ((oflag & O_TRUNC) && (oflag & O_ACCMODE) == O_RDONLY)
        oflag = (oflag & ~O_ACCMODE) | O_RDWR;
    fd = open(hp, oflag, 0666);
    if (fd < 0) {
        seterr(errno == EEXIST ? 80 : werr(errno));
        return INVALID;
    }
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        seterr(5);
        return INVALID;
    }
    h = hfile(fd);
    hget(h)->path = strdup(hp);
    hget(h)->delete_on_close = (flags & 0x04000000u) != 0;
    seterr(0);
    return h;
}

/* at the end of the program: remove the files that were to be deleted when closed and are open still */
static void exit_files(void)
{
    views_sync();
    for (u32 i = 0; i < NHND; i++)
        if (H[i].type == HT_FILE && H[i].delete_on_close && H[i].path)
            unlink(H[i].path);
}

static void hclose(struct hnd *x)
{
    if ((x->type == HT_FILE || x->type == HT_MAP) && x->fd >= 0)
        views_sync_fd(x->fd);
    switch (x->type) {
    case HT_FILE:
    case HT_MAP:
    case HT_PROC:
        if (x->fd > 2)
            close(x->fd);
        if (x->delete_on_close && x->path)
            unlink(x->path);
        break;
    case HT_FIND:
        for (int i = 0; i < x->n; i++)
            free(x->names[i]);
        free(x->names);
        free(x->dir);
        break;
    }
    free(x->path);
    memset(x, 0, sizeof *x);
}

API(CloseHandle)
{
    struct hnd *x = hget(a[0]);
    if (!x) {
        seterr(6);
        return 0;
    }
    hclose(x);
    return 1;
}

API(ReadFile)
{
    int fd = hfd(a[0]), blk, e;
    struct thread *me = cur;

    views_sync_fd(fd);
    struct stat st;
    ssize_t n;

    if (a[3])
        w32(a[3], 0);
    if (fd < 0) {
        seterr(6);
        return 0;
    }
    /* a pipe or a terminal can make the read wait: the other threads run in that time */
    blk = multi && fstat(fd, &st) == 0 && !S_ISREG(st.st_mode);
    if (blk)
        cpu_give();
    do
        n = read(fd, G + a[1], a[2]);
    while (n < 0 && errno == EINTR);
    e = errno;
    if (blk)
        cpu_take(me);
    if (n < 0) {
        seterr(werr(e));
        return 0;
    }
    if (n == 0 && a[2] && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
        seterr(109); /* the pipe has ended: the other side closed it */
        return 0;
    }
    if (a[3])
        w32(a[3], (u32)n);
    return 1;
}

API(WriteFile)
{
    int fd = hfd(a[0]);
    u32 done = 0;

    views_sync_fd(fd);

    if (a[3])
        w32(a[3], 0);
    if (fd < 0) {
        seterr(6);
        return 0;
    }
    while (done < a[2]) {
        ssize_t n = write(fd, G + a[1] + done, a[2] - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            seterr(werr(errno));
            return 0;
        }
        done += (u32)n;
    }
    if (a[3])
        w32(a[3], done);
    return 1;
}

API(SetFilePointer)
{
    int fd = hfd(a[0]);
    s64 off = (s32)a[1];
    off_t r;

    if (fd < 0) {
        seterr(6);
        return INVALID;
    }
    if (a[2])
        off = (s64)(((u64)r32(a[2]) << 32) | a[1]);
    r = lseek(fd, off, a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) {
        seterr(errno == ESPIPE ? 132 : 131);
        return INVALID;
    }
    if (a[2])
        w32(a[2], (u32)((u64)r >> 32));
    seterr(0);
    return (u32)r;
}

API(SetEndOfFile)
{
    int fd = hfd(a[0]);
    off_t pos = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(fd, pos) < 0) {
        seterr(fd < 0 ? 6 : werr(errno));
        return 0;
    }
    return 1;
}

API(FlushFileBuffers) { UNUSED; return 1; }
API(LockFile) { UNUSED; return 1; }
API(UnlockFile) { UNUSED; return 1; }

API(GetFileSize)
{
    struct stat st;
    int fd = hfd(a[0]);
    if (fd < 0 || fstat(fd, &st) < 0) {
        seterr(6);
        return INVALID;
    }
    if (a[1])
        w32(a[1], (u32)((u64)st.st_size >> 32));
    seterr(0);
    return (u32)st.st_size;
}

static u32 attrs_of(const struct stat *st)
{
    u32 at = S_ISDIR(st->st_mode) ? 0x10 : 0x20;
    if (!(st->st_mode & 0222))
        at |= 1;
    return at;
}

API(GetFileAttributesA)
{
    char hp[PATH_MAX];
    struct stat st;
    to_host(gstr(a[0]), hp, sizeof hp);
    if (stat(hp, &st) < 0) {
        seterr(werr(errno));
        return INVALID;
    }
    return attrs_of(&st);
}

API(SetFileAttributesA)
{
    char hp[PATH_MAX];
    struct stat st;
    to_host(gstr(a[0]), hp, sizeof hp);
    if (stat(hp, &st) < 0) {
        seterr(werr(errno));
        return 0;
    }
    if (a[1] & 1)
        chmod(hp, st.st_mode & ~0222u);
    else
        chmod(hp, st.st_mode | 0200);
    return 1;
}

API(GetFileTime)
{
    struct stat st;
    int fd = hfd(a[0]);
    if (fd < 0 || fstat(fd, &st) < 0) {
        seterr(6);
        return 0;
    }
    if (a[1]) w64(a[1], unix_to_ft(st.st_mtime, 0));
    if (a[2]) w64(a[2], unix_to_ft(st.st_atime, 0));
    if (a[3]) w64(a[3], unix_to_ft(st.st_mtime, 0));
    return 1;
}

API(SetFileTime)
{
    int fd = hfd(a[0]);
    struct stat st;
    struct timeval tv[2];
    if (fd < 0 || fstat(fd, &st) < 0) {
        seterr(6);
        return 0;
    }
    tv[0].tv_sec = a[2] ? ft_to_unix(r64(a[2])) : st.st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = a[3] ? ft_to_unix(r64(a[3])) : st.st_mtime;
    tv[1].tv_usec = 0;
    futimes(fd, tv);
    return 1;
}

API(GetFileInformationByHandle)
{
    struct stat st;
    int fd = hfd(a[0]);
    u32 p = a[1];
    if (fd < 0 || fstat(fd, &st) < 0) {
        seterr(6);
        return 0;
    }
    memset(G + p, 0, 52);
    w32(p, attrs_of(&st));
    w64(p + 4, unix_to_ft(st.st_mtime, 0));
    w64(p + 12, unix_to_ft(st.st_atime, 0));
    w64(p + 20, unix_to_ft(st.st_mtime, 0));
    w32(p + 28, (u32)st.st_dev);
    w32(p + 32, (u32)((u64)st.st_size >> 32));
    w32(p + 36, (u32)st.st_size);
    w32(p + 40, (u32)st.st_nlink);
    w32(p + 44, (u32)((u64)st.st_ino >> 32));
    w32(p + 48, (u32)st.st_ino);
    return 1;
}

API(DeleteFileA)
{
    char hp[PATH_MAX];
    to_host(gstr(a[0]), hp, sizeof hp);
    if (unlink(hp) < 0) {
        seterr(werr(errno));
        return 0;
    }
    return 1;
}

API(MoveFileA)
{
    char h1[PATH_MAX], h2[PATH_MAX];
    struct stat st;
    to_host(gstr(a[0]), h1, sizeof h1);
    to_host(gstr(a[1]), h2, sizeof h2);
    if (stat(h2, &st) == 0) {
        seterr(183);
        return 0;
    }
    if (rename(h1, h2) < 0) {
        seterr(werr(errno));
        return 0;
    }
    return 1;
}

API(CreateDirectoryA)
{
    char hp[PATH_MAX];
    to_host(gstr(a[0]), hp, sizeof hp);
    if (mkdir(hp, 0777) < 0) {
        seterr(errno == EEXIST ? 183 : werr(errno));
        return 0;
    }
    return 1;
}

API(RemoveDirectoryA)
{
    char hp[PATH_MAX];
    to_host(gstr(a[0]), hp, sizeof hp);
    if (rmdir(hp) < 0) {
        seterr(werr(errno));
        return 0;
    }
    return 1;
}

API(GetCurrentDirectoryA)
{
    char cwd[PATH_MAX], g[PATH_MAX];
    u32 n;
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "/");
    to_guest(cwd, g, sizeof g);
    n = (u32)strlen(g);
    if (n + 1 > a[0])
        return n + 1;
    memcpy(G + a[1], g, n + 1);
    return n;
}

API(SetCurrentDirectoryA)
{
    char hp[PATH_MAX];
    to_host(gstr(a[0]), hp, sizeof hp);
    if (chdir(hp) < 0) {
        seterr(werr(errno));
        return 0;
    }
    return 1;
}

API(GetFullPathNameA)
{
    char g[PATH_MAX];
    u32 n;
    full_guest(gstr(a[0]), g, sizeof g);
    n = (u32)strlen(g);
    if (n + 1 > a[1])
        return n + 1;
    memcpy(G + a[2], g, n + 1);
    if (a[3]) {
        char *bs = strrchr(g, '\\');
        w32(a[3], bs && bs[1] ? a[2] + (u32)(bs + 1 - g) : 0);
    }
    return n;
}

API(GetDriveTypeA) { UNUSED; return 3; }
API(GetLogicalDrives)
{
    u32 m = 1u << (root_letter - 'A');
    UNUSED;
    for (int i = 0; i < 26; i++)
        if (drive_dir[i])
            m |= 1u << i;
    return m;
}
API(GetDiskFreeSpaceA)
{
    if (a[1]) w32(a[1], 8);
    if (a[2]) w32(a[2], 512);
    if (a[3]) w32(a[3], 0x100000);
    if (a[4]) w32(a[4], 0x200000);
    return 1;
}

static int wild(const char *pat, const char *s)
{
    for (; *pat; pat++) {
        if (*pat == '*') {
            while (pat[1] == '*')
                pat++;
            if (!pat[1])
                return 1;
            for (; *s; s++)
                if (wild(pat + 1, s))
                    return 1;
            return wild(pat + 1, s);
        }
        if (*pat == '?') {
            /* one character; at the end of the name or before a "." it can be for nothing */
            if (*s && *s != '.')
                s++;
            continue;
        }
        if (!*s || tolower((unsigned char)*pat) != tolower((unsigned char)*s))
            return 0;
        s++;
    }
    return !*s;
}

/* Does the file name NAME fit the pattern PAT, by the rules of DOS and Windows?
   "*.*" is for all names; a name with no "." is as if it had one at its end,
   so "*." is for the names with no extension and "FOO.*" is for "FOO" too. */
int wild_name(const char *pat, const char *name)
{
    char tmp[NAME_MAX + 2];

    if (!strcmp(pat, "*.*") || !strcmp(pat, "*"))
        return 1;
    if (wild(pat, name))
        return 1;
    if (strchr(name, '.') || strlen(name) > NAME_MAX)
        return 0;
    snprintf(tmp, sizeof tmp, "%s.", name);
    return wild(pat, tmp);
}

static void put_find(struct hnd *x, u32 p)
{
    char full[PATH_MAX];
    struct stat st;
    const char *name = x->names[x->pos++];

    memset(G + p, 0, 320);
    snprintf(full, sizeof full, "%s/%s", x->dir, name);
    if (stat(full, &st) == 0) {
        w32(p, attrs_of(&st));
        w64(p + 4, unix_to_ft(st.st_mtime, 0));
        w64(p + 12, unix_to_ft(st.st_atime, 0));
        w64(p + 20, unix_to_ft(st.st_mtime, 0));
        w32(p + 28, (u32)((u64)st.st_size >> 32));
        w32(p + 32, (u32)st.st_size);
    } else {
        w32(p, 0x80);
    }
    snprintf((char *)G + p + 44, 260, "%s", name);
}

API(FindFirstFileA)
{
    char g[PATH_MAX], hdir[PATH_MAX], *bs, *pat;
    DIR *d;
    struct dirent *e;
    struct hnd *x;
    u32 h;

    snprintf(g, sizeof g, "%s", gstr(a[0]));
    for (char *s = g; *s; s++)
        if (*s == '/')
            *s = '\\';
    bs = strrchr(g, '\\');
    if (bs) {
        *bs = 0;
        pat = bs + 1;
        to_host(g[0] && !(g[1] == ':' && !g[2]) ? g : "\\", hdir, sizeof hdir);
    } else if (g[0] && g[1] == ':') {
        pat = g + 2;
        snprintf(hdir, sizeof hdir, ".");
    } else {
        pat = g;
        snprintf(hdir, sizeof hdir, ".");
    }
    d = opendir(hdir);
    if (!d) {
        seterr(3);
        return INVALID;
    }
    h = hnew(HT_FIND);
    x = hget(h);
    x->dir = strdup(hdir);
    while ((e = readdir(d))) {
        if (!wild_name(pat, e->d_name))
            continue;
        x->names = realloc(x->names, sizeof *x->names * (size_t)(x->n + 1));
        x->names[x->n++] = strdup(e->d_name);
    }
    closedir(d);
    if (!x->n) {
        hclose(x);
        seterr(2);
        return INVALID;
    }
    put_find(x, a[1]);
    return h;
}

API(FindNextFileA)
{
    struct hnd *x = hget(a[0]);
    if (!x || x->type != HT_FIND) {
        seterr(6);
        return 0;
    }
    if (x->pos >= x->n) {
        seterr(18);
        return 0;
    }
    put_find(x, a[1]);
    return 1;
}

API(FindClose) { return k_CloseHandle(a); }

/* console */
API(GetConsoleMode)
{
    int fd = hfd(a[0]);
    if (fd < 0 || !isatty(fd)) {
        seterr(6);
        return 0;
    }
    w32(a[1], 7);
    return 1;
}
API(SetConsoleMode) { int fd = hfd(a[0]); return fd >= 0 && isatty(fd); }
API(GetConsoleScreenBufferInfo)
{
    int fd = hfd(a[0]);
    if (fd < 0 || !isatty(fd)) {
        seterr(6);
        return 0;
    }
    memset(G + a[1], 0, 22);
    w16(a[1], 80);
    w16(a[1] + 2, 25);
    w16(a[1] + 8, 7);
    w16(a[1] + 14, 79);
    w16(a[1] + 16, 24);
    w16(a[1] + 18, 80);
    w16(a[1] + 20, 25);
    return 1;
}
API(WriteConsoleA) { u32 b[5] = { a[0], a[1], a[2], a[3], 0 }; return k_WriteFile(b); }
API(ReadConsoleA) { u32 b[5] = { a[0], a[1], a[2], a[3], 0 }; return k_ReadFile(b); }

/* strings and locale: one byte for each character, Latin-1 */
API(MultiByteToWideChar)
{
    const u8 *s = G + a[2];
    s32 n = (s32)a[3];
    if (n < 0)
        n = (s32)strlen((const char *)s) + 1;
    if (!a[5])
        return (u32)n;
    if ((u32)n > a[5]) {
        seterr(122);
        return 0;
    }
    for (s32 i = 0; i < n; i++)
        w16(a[4] + (u32)i * 2, s[i]);
    return (u32)n;
}

API(WideCharToMultiByte) /* (code page, flags, text, length, buffer, size, default character, was it used) */
{
    s32 n = (s32)a[3];
    u32 used = 0;
    u8 def = a[6] ? r8(a[6]) : '?';
    if (n < 0) {
        for (n = 0; r16(a[2] + (u32)n * 2); n++)
            ;
        n++;
    }
    if (a[5] && (u32)n > a[5]) {
        seterr(122);
        return 0;
    }
    for (s32 i = 0; i < n; i++) {
        u16 c = r16(a[2] + (u32)i * 2);
        if (c >= 256)
            used = 1;
        if (a[5])
            w8(a[4] + (u32)i, c < 256 ? (u8)c : def);
    }
    if (a[7]) /* the C library of 2005 takes a character as not written when this is not 0 */
        w32(a[7], used);
    return (u32)n;
}

static u16 ctype1(u32 c)
{
    u16 t = 0;
    if (c >= 0x80) {
        if (c >= 0xc0 && c != 0xd7 && c != 0xf7 && c < 0x100)
            t = (u16)(0x100 | (c >= 0xe0 || c == 0xdf ? 2 : 1));
        else if (c == 0xa0)
            t = 0x48;
        else if (c < 0xa0)
            t = 0x20;
        else
            t = 0x10;
        return t;
    }
    if (isupper((int)c)) t |= 0x101;
    if (islower((int)c)) t |= 0x102;
    if (isdigit((int)c)) t |= 4;
    if (isspace((int)c)) t |= 8;
    if (ispunct((int)c)) t |= 0x10;
    if (iscntrl((int)c)) t |= 0x20;
    if (c == ' ' || c == '\t') t |= 0x40;
    if (isxdigit((int)c)) t |= 0x80;
    return t;
}

API(GetStringTypeA)
{
    s32 n = (s32)a[3];
    if (n < 0)
        n = (s32)strlen(gstr(a[2])) + 1;
    for (s32 i = 0; i < n; i++)
        w16(a[4] + (u32)i * 2, a[1] == 1 ? ctype1(r8(a[2] + (u32)i)) : 0);
    return 1;
}

API(GetStringTypeW)
{
    s32 n = (s32)a[2];
    if (n < 0) {
        for (n = 0; r16(a[1] + (u32)n * 2); n++)
            ;
        n++;
    }
    for (s32 i = 0; i < n; i++)
        w16(a[3] + (u32)i * 2, a[0] == 1 ? ctype1(r16(a[1] + (u32)i * 2)) : 0);
    return 1;
}

static u32 mapchar(u32 c, u32 flags)
{
    if (flags & 0x100) { /* lower */
        if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
            return c + 32;
    } else if (flags & 0x200) { /* upper */
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
            return c - 32;
    }
    return c;
}

API(LCMapStringA)
{
    s32 n = (s32)a[3];
    if (n < 0)
        n = (s32)strlen(gstr(a[2])) + 1;
    if (!a[5])
        return (u32)n;
    if ((u32)n > a[5]) {
        seterr(122);
        return 0;
    }
    for (s32 i = 0; i < n; i++)
        w8(a[4] + (u32)i, (u8)mapchar(r8(a[2] + (u32)i), a[1]));
    return (u32)n;
}

API(LCMapStringW)
{
    s32 n = (s32)a[3];
    if (n < 0) {
        for (n = 0; r16(a[2] + (u32)n * 2); n++)
            ;
        n++;
    }
    if (!a[5])
        return (u32)n;
    if ((u32)n > a[5]) {
        seterr(122);
        return 0;
    }
    for (s32 i = 0; i < n; i++)
        w16(a[4] + (u32)i * 2, (u16)mapchar(r16(a[2] + (u32)i * 2), a[1]));
    return (u32)n;
}

API(CompareStringA)
{
    const u8 *s1 = G + a[2], *s2 = G + a[4];
    s32 n1 = (s32)a[3], n2 = (s32)a[5];
    int ic = a[1] & 1;
    if (n1 < 0) n1 = (s32)strlen((const char *)s1);
    if (n2 < 0) n2 = (s32)strlen((const char *)s2);
    for (s32 i = 0; i < n1 && i < n2; i++) {
        u32 c1 = s1[i], c2 = s2[i];
        if (ic) { c1 = mapchar(c1, 0x200); c2 = mapchar(c2, 0x200); }
        if (c1 != c2)
            return c1 < c2 ? 1 : 3;
    }
    return n1 == n2 ? 2 : n1 < n2 ? 1 : 3;
}

API(GetLocaleInfoA)
{
    const char *s = "";
    u32 n;
    switch (a[1] & 0xffff) {
    case 0x1001: s = "English"; break;
    case 0x1002: s = "United States"; break;
    case 0x0003: s = "ENU"; break;
    case 0x0007: s = "USA"; break;
    case 0x000e: s = "."; break;
    case 0x000f: s = ","; break;
    case 0x0010: s = "3;0"; break;
    case 0x1004: s = "1252"; break;
    case 0x000b: s = "437"; break;
    case 0x001d: s = "/"; break;
    case 0x001e: s = ":"; break;
    case 0x001f: s = "M/d/yy"; break;
    case 0x0020: s = "dddd, MMMM dd, yyyy"; break;
    case 0x1003: s = "h:mm:ss tt"; break;
    case 0x0028: s = "AM"; break;
    case 0x0029: s = "PM"; break;
    }
    n = (u32)strlen(s) + 1;
    if (!a[3])
        return n;
    if (n > a[3]) {
        seterr(122);
        return 0;
    }
    memcpy(G + a[2], s, n);
    return n;
}

/* time */
API(GetSystemTime)
{
    struct timeval tv;
    struct tm tm;
    time_t t;
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    gmtime_r(&t, &tm);
    put_systemtime(a[0], &tm, (int)(tv.tv_usec / 1000));
    return 0;
}

API(GetLocalTime)
{
    struct timeval tv;
    struct tm tm;
    time_t t;
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    localtime_r(&t, &tm);
    put_systemtime(a[0], &tm, (int)(tv.tv_usec / 1000));
    return 0;
}

API(SetLocalTime) { UNUSED; seterr(5); return 0; }

/* seconds east of UTC for the broken-down local time TM (the zone, with summer time) */
static long host_gmtoff(const struct tm *tm)
{
#ifdef _WIN32
    /* the Windows struct tm has no tm_gmtoff */
    return nt_gmtoff(tm);
#else
    return tm->tm_gmtoff;
#endif
}

API(GetTimeZoneInformation)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    memset(G + a[0], 0, 172);
    w32(a[0], (u32)(s32)(-host_gmtoff(&tm) / 60));
    return 0;
}

API(FileTimeToSystemTime)
{
    u64 ft = r64(a[0]);
    time_t t = ft_to_unix(ft);
    struct tm tm;
    gmtime_r(&t, &tm);
    put_systemtime(a[1], &tm, (int)(ft / 10000 % 1000));
    return 1;
}

API(SystemTimeToFileTime)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = r16(a[0]) - 1900;
    tm.tm_mon = r16(a[0] + 2) - 1;
    tm.tm_mday = r16(a[0] + 6);
    tm.tm_hour = r16(a[0] + 8);
    tm.tm_min = r16(a[0] + 10);
    tm.tm_sec = r16(a[0] + 12);
    w64(a[1], unix_to_ft(timegm(&tm), (long)r16(a[0] + 14) * 1000000));
    return 1;
}

static s64 local_offset(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (s64)host_gmtoff(&tm) * 10000000ll;
}

API(FileTimeToLocalFileTime)
{
    u64 ft = r64(a[0]);
    w64(a[1], ft + (u64)local_offset(ft_to_unix(ft)));
    return 1;
}

API(LocalFileTimeToFileTime)
{
    u64 ft = r64(a[0]);
    w64(a[1], ft - (u64)local_offset(ft_to_unix(ft)));
    return 1;
}

/* thread local storage, locks: one thread */
/* thread-local storage: each thread has its own value in a slot */
static u64 tls_used = 1;
API(TlsAlloc)
{
    UNUSED;
    for (int i = 1; i < 64; i++)
        if (!(tls_used & (1ull << i))) {
            tls_used |= 1ull << i;
            for (int k = 0; k < NTHREADS; k++)
                threads[k].tls[i] = 0;
            return (u32)i;
        }
    seterr(8);
    return INVALID;
}
API(TlsFree) { if (a[0] < 64) tls_used &= ~(1ull << a[0]); return 1; }
API(TlsGetValue) { seterr(0); return a[0] < 64 ? cur->tls[a[0]] : 0; }
API(TlsSetValue) { if (a[0] < 64) cur->tls[a[0]] = a[1]; return 1; }

/* A critical section, in the program's memory: +4 lock count (-1: free), +8 how many times
   its thread has entered it, +12 the thread that has it. */
static int thread_alive(u32 id)
{
    for (int k = 0; k < NTHREADS; k++)
        if (threads[k].used && !threads[k].done && threads[k].id == id)
            return 1;
    return 0;
}

API(InitializeCriticalSection)
{
    memset(G + a[0], 0, 24);
    w32(a[0] + 4, 0xffffffff);
    return 0;
}
API(DeleteCriticalSection) { UNUSED; return 0; }
API(EnterCriticalSection)
{
    u32 cs = a[0], owner;

    /* wait while another thread has it; with one thread there is no other */
    for (int n = 0; multi && (owner = r32(cs + 12)) != 0 && owner != cur->id && thread_alive(owner); n++)
        thread_wait(n);
    if (r32(cs + 12) != cur->id) {
        w32(cs + 12, cur->id);
        w32(cs + 8, 0);
    }
    w32(cs + 8, r32(cs + 8) + 1);
    w32(cs + 4, r32(cs + 4) + 1);
    return 0;
}
API(TryEnterCriticalSection)
{
    u32 owner = r32(a[0] + 12);

    if (multi && owner && owner != cur->id && thread_alive(owner))
        return 0;
    k_EnterCriticalSection(a);
    return 1;
}
API(LeaveCriticalSection)
{
    u32 cs = a[0], n = r32(cs + 8);

    if (n)
        n--;
    w32(cs + 8, n);
    w32(cs + 4, r32(cs + 4) - 1);
    if (!n)
        w32(cs + 12, 0);
    return 0;
}

/* events, mutexes and semaphores */
enum { SY_EVENT = 1, SY_MUTEX, SY_SEM };

API(CreateSemaphoreA) /* (security, count, highest count, name) */
{
    u32 h = hnew(HT_SYNC);
    hget(h)->kind = SY_SEM;
    hget(h)->count = (s32)a[1];
    hget(h)->max = (s32)a[2];
    return h;
}
API(ReleaseSemaphore)
{
    struct hnd *x = hget(a[0]);
    if (!x || x->type != HT_SYNC) {
        seterr(6);
        return 0;
    }
    if (x->max > 0 && x->count + (s32)a[1] > x->max) {
        seterr(298); /* too many posts were made to a semaphore */
        return 0;
    }
    if (a[2])
        w32(a[2], (u32)x->count);
    x->count += (s32)a[1];
    return 1;
}
API(CreateMutexA) /* (security, the thread has it from the start, name) */
{
    u32 h = hnew(HT_SYNC);
    hget(h)->kind = SY_MUTEX;
    if (a[1]) {
        hget(h)->owner = cur->id;
        hget(h)->count = 1;
    }
    return h;
}
API(ReleaseMutex)
{
    struct hnd *x = hget(a[0]);
    if (!x || x->type != HT_SYNC) {
        seterr(6);
        return 0;
    }
    if (x->kind == SY_MUTEX) {
        if (x->owner != cur->id) {
            seterr(288); /* the thread does not have the mutex */
            return 0;
        }
        if (--x->count <= 0) {
            x->count = 0;
            x->owner = 0;
        }
    }
    return 1;
}

API(RtlMoveMemory) { memmove(G + a[0], G + a[1], a[2]); return 0; }
API(RtlFillMemory) { memset(G + a[0], (int)a[2], a[1]); return 0; }
API(RtlZeroMemory) { memset(G + a[0], 0, a[1]); return 0; }

/* modules */
API(LoadLibraryA)
{
    char name[PATH_MAX], hp[PATH_MAX];
    const char *base;
    struct module *m;

    snprintf(name, sizeof name, "%s", gstr(a[0]));
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    m = NULL;
    if (base != name) { /* a path */
        char up[64];
        snprintf(up, sizeof up, "%s", base);
        for (char *s = up; *s; s++)
            *s = (char)toupper((unsigned char)*s);
        if (!strchr(up, '.'))
            strcat(up, ".DLL");
        for (int i = 0; i < nmods; i++)
            if (!strcmp(mods[i].name, up))
                m = &mods[i];
        if (!m) {
            to_host(name, hp, sizeof hp);
            if (access(hp, R_OK) == 0)
                m = load_image(hp, up);
        }
    } else {
        m = get_module(base);
    }
    if (!m) {
        seterr(126);
        return 0;
    }
    init_dlls();
    return m->base;
}

API(FreeLibrary) { UNUSED; return 1; }

API(GetProcAddress)
{
    for (int i = 0; i < nmods; i++)
        if (mods[i].base == a[0]) {
            u32 addr = 0;
            if (mods[i].builtin) {
                /* only a function that ntrun has: a program asks for a name to learn if the system
                   has it (LINK asks KERNEL32 for "IsTNT" to see if it runs on a DOS extender) */
                if (a[1] >= 0x10000)
                    for (int k = 1; k < napis; k++)
                        if (apis[k].fn && !strcmp(apis[k].name, gstr(a[1]))) {
                            addr = api_thunk(apis[k].name);
                            break;
                        }
                if (!addr && trace_api && a[1] >= 0x10000)
                    fprintf(stderr, "[%d] GetProcAddress: ntrun does not have %s\n", (int)getpid(), gstr(a[1]));
            } else {
                addr = a[1] < 0x10000 ? resolve_export(&mods[i], NULL, a[1]) : resolve_export(&mods[i], gstr(a[1]), 0);
            }
            if (!addr)
                seterr(127);
            return addr;
        }
    seterr(126);
    return 0;
}

API(GetModuleHandleA)
{
    struct module *m;
    if (!a[0])
        return mods[0].base;
    {
        char up[64];
        const char *b = strrchr(gstr(a[0]), '\\');
        snprintf(up, sizeof up, "%s", b ? b + 1 : gstr(a[0]));
        for (char *s = up; *s; s++)
            *s = (char)toupper((unsigned char)*s);
        if (!strchr(up, '.'))
            strcat(up, ".DLL");
        for (int i = 0; i < nmods; i++)
            if (!strcmp(mods[i].name, up))
                return mods[i].base;
        m = is_builtin(up) ? get_module(up) : NULL;
    }
    if (!m)
        seterr(126);
    return m ? m->base : 0;
}

API(GetModuleFileNameA)
{
    char g[PATH_MAX];
    const char *hp = exe_hpath;
    u32 n;
    if (a[0])
        for (int i = 0; i < nmods; i++)
            if (mods[i].base == a[0] && mods[i].hpath[0])
                hp = mods[i].hpath;
    to_guest(hp, g, sizeof g);
    n = (u32)strlen(g);
    if (n + 1 > a[2])
        n = a[2] ? a[2] - 1 : 0;
    memcpy(G + a[1], g, n);
    w8(a[1] + n, 0);
    return n;
}

/* file mappings */
static struct {
    u32 addr, len, off;
    int fd;
    int copy;   /* Windows host, no mapping: a copy in memory that is written back to the file */
    int mapped; /* Windows host: a real view of the file at that address (MapViewOfFileEx) */
    u64 vol, idx; /* which file (a copy) */
} views[256];
static int nviews;

#ifdef _WIN32
/* the number of the file of a file descriptor: the same file has the same number */
static int file_number(int fd, u64 *vol, u64 *idx)
{
    BY_HANDLE_FILE_INFORMATION i;

    if (!GetFileInformationByHandle((HANDLE)_get_osfhandle(fd), &i))
        return 0;
    *vol = i.dwVolumeSerialNumber;
    *idx = ((u64)i.nFileIndexHigh << 32) | i.nFileIndexLow;
    return 1;
}

/* A view of a file that can be written is a real view when the host can map the file at
   the address of the guest. Where it cannot (the file is not open for writing, the offset
   is not a multiple of 64 KB), the view is a copy in memory: the file gets the copy back
   when the program flushes, reads or writes the file, closes it or ends. */
static void views_sync_one(int i)
{
    if (views[i].copy && views[i].fd >= 0)
        pwrite(views[i].fd, G + views[i].addr, views[i].len, views[i].off);
}

static void views_sync(void)
{
    for (int i = 0; i < nviews; i++)
        views_sync_one(i);
}

static void views_sync_fd(int fd)
{
    u64 vol, idx;

    if (fd < 0 || !nviews || !file_number(fd, &vol, &idx))
        return;
    for (int i = 0; i < nviews; i++)
        if (views[i].copy && views[i].vol == vol && views[i].idx == idx)
            views_sync_one(i);
}

/* map the file of FD at the guest address ADDR (regions of 64 KB that region_alloc gave) */
static int map_view(int fd, u32 addr, u32 len, u32 off)
{
    HANDLE sec;
    u32 n = (len + CHUNK - 1) / CHUNK;
    void *p;

    if (off & (CHUNK - 1)) /* a view starts at a multiple of 64 KB */
        return 0;
    sec = CreateFileMappingA((HANDLE)_get_osfhandle(fd), NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!sec)
        return 0;
    for (u32 k = 0; k < n; k++)
        VirtualFree(G + addr + (size_t)k * CHUNK, 0, MEM_RELEASE); /* the places are free now */
    p = MapViewOfFileEx(sec, FILE_MAP_WRITE, 0, off, len, G + addr);
    CloseHandle(sec); /* the view keeps the file mapping */
    if (p == G + addr)
        return 1;
    if (p)
        UnmapViewOfFile(p);
    host_decommit(addr, n);
    host_commit(addr, n);
    return 0;
}
#else
static void views_sync(void) {}
static void views_sync_fd(int fd) { (void)fd; }
#endif

API(CreateFileMappingA)
{
    u32 h;
    struct hnd *x;
    struct stat st;
    int fd = -1;
    u32 size = a[4];

    if (a[0] != INVALID) {
        int ffd = hfd(a[0]);
        if (ffd < 0 || fstat(ffd, &st) < 0) {
            seterr(6);
            return 0;
        }
        fd = dup(ffd);
        if (!size)
            size = (u32)st.st_size;
        else if (size > (u64)st.st_size && (a[2] & 0xff) == 4)
            ftruncate(fd, size);
        if (!size) {
            close(fd);
            seterr(1006);
            return 0;
        }
    }
    h = hnew(HT_MAP);
    x = hget(h);
    x->fd = fd;
    x->size = size;
    x->prot = a[2];
    seterr(0);
    return h;
}

API(MapViewOfFileEx)
{
    struct hnd *x = hget(a[0]);
    u32 off = a[3], len = a[4], addr;
    int shared = 0, mapped = 0;

    if (!x || x->type != HT_MAP) {
        seterr(6);
        return 0;
    }
    if (!len)
        len = x->size - off;
    addr = 0;
#ifdef _WIN32
    /* a view of a file that the program can write goes to the zone, where it can be mapped */
    if (!a[5] && x->fd >= 0 && (a[1] & 2))
        addr = region_alloc_zone(len);
#endif
    if (!addr)
        addr = region_alloc(a[5], len);
    if (!addr) {
        seterr(487);
        return 0;
    }
    if (x->fd >= 0) {
        shared = (a[1] & 2) != 0; /* FILE_MAP_WRITE; FILE_MAP_ALL_ACCESS has it too */
#ifdef _WIN32
        if (shared && IN_ZONE(addr / CHUNK))
            mapped = map_view(x->fd, addr, len, off);
        if (!mapped) {
            /* a private view, or one that cannot be mapped: a copy. A copy that can be written
               is written back to the file (views_sync) */
            if (pread(x->fd, G + addr, len, off) < 0) {
                region_free(addr, len);
                seterr(werr(errno));
                return 0;
            }
        }
#else
        size_t hlen, ps = (size_t)getpagesize();
        hlen = ((size_t)len + ps - 1) & ~(ps - 1);
        if ((off % ps) != 0 || mmap(G + addr, hlen, PROT_READ | PROT_WRITE, MAP_FIXED | (shared ? MAP_SHARED : MAP_PRIVATE), x->fd, off) == MAP_FAILED) {
            /* cannot map: read it */
            if (shared)
                die("MapViewOfFile: cannot map the file for writing: %s", strerror(errno));
            if (pread(x->fd, G + addr, len, off) < 0) {
                region_free(addr, len);
                seterr(werr(errno));
                return 0;
            }
        }
#endif
    }
    if (nviews == 256)
        die("too many file views");
    views[nviews].addr = addr;
    views[nviews].len = len;
    views[nviews].off = off;
    views[nviews].mapped = mapped;
    views[nviews].copy = 0;
    views[nviews].fd = -1;
#ifdef _WIN32
    if (shared && !mapped && file_number(x->fd, &views[nviews].vol, &views[nviews].idx)) {
        views[nviews].copy = 1;
        views[nviews].fd = dup(x->fd); /* the view outlives the handle */
    }
#endif
    nviews++;
    return addr;
}

API(MapViewOfFile) { u32 b[6] = { a[0], a[1], a[2], a[3], a[4], 0 }; return k_MapViewOfFileEx(b); }

API(UnmapViewOfFile)
{
    for (int i = 0; i < nviews; i++)
        if (views[i].addr == a[0]) {
#ifdef _WIN32
            views_sync_one(i);
            if (views[i].fd >= 0)
                close(views[i].fd);
            if (views[i].mapped)
                UnmapViewOfFile(G + views[i].addr);
#endif
            region_free(views[i].addr, views[i].len);
            views[i] = views[--nviews];
            return 1;
        }
    seterr(487);
    return 0;
}

/* processes */

#ifdef _WIN32
/* A child ntrun is a new process of the host (there is no fork) with the environment that
   its program gets. The exit code comes whole from the process, no pipe for it. */
struct envv { char **v; int n; };

static size_t envv_namelen(const char *t)
{
    const char *eq = strchr(t + (t[0] == '='), '=');
    return eq ? (size_t)(eq - t) : strlen(t);
}

static void envv_unset(struct envv *e, const char *name, size_t len)
{
    for (int i = 0; i < e->n; i++)
        if (envv_namelen(e->v[i]) == len && !_strnicmp(e->v[i], name, len)) {
            free(e->v[i]);
            e->v[i] = e->v[--e->n];
            i--;
        }
}

static void envv_set(struct envv *e, const char *text)
{
    envv_unset(e, text, envv_namelen(text));
    e->v = realloc(e->v, sizeof *e->v * (size_t)(e->n + 2));
    e->v[e->n++] = strdup(text);
}

static void envv_init(struct envv *e)
{
    e->v = NULL;
    e->n = 0;
    for (char **p = environ; p && *p; p++) {
        e->v = realloc(e->v, sizeof *e->v * (size_t)(e->n + 2));
        e->v[e->n++] = strdup(*p);
    }
}

static void envv_free(struct envv *e)
{
    for (int i = 0; i < e->n; i++)
        free(e->v[i]);
    free(e->v);
}

/* start ntrun with the program HP; the environment is E. MODE: _P_WAIT (the result is the exit
   code) or _P_NOWAIT (the handle of the process). -1: it did not start */
static intptr_t spawn_ntrun(int mode, const char *hp, struct envv *e)
{
    /* _spawnve does not work with this C library: the environment of this process is the new
       one while the child starts, then it is as it was */
    char a0[PATH_MAX + 4], a1[PATH_MAX + 4], *argv[3] = { a0, a1, NULL };
    struct envv old;
    intptr_t r;

    snprintf(a0, sizeof a0, "\"%s\"", self_path);
    snprintf(a1, sizeof a1, "\"%s\"", hp);
    envv_init(&old);
    for (int i = 0; i < old.n; i++) {
        size_t l = envv_namelen(old.v[i]);
        int keep = 0;
        if (old.v[i][0] == '=')
            continue;
        for (int k = 0; k < e->n && !keep; k++)
            keep = envv_namelen(e->v[k]) == l && !_strnicmp(e->v[k], old.v[i], l);
        if (!keep) {
            char name[512];
            snprintf(name, sizeof name, "%.*s", (int)l, old.v[i]);
            _putenv_s(name, "");
        }
    }
    for (int i = 0; i < e->n; i++)
        if (e->v[i][0] != '=')
            _putenv(e->v[i]);
    fflush(stdout);
    fflush(stderr);
    r = _spawnv(mode, self_path, (const char *const *)argv);
    for (int i = 0; i < e->n; i++) {
        size_t l = envv_namelen(e->v[i]);
        int keep = 0;
        if (e->v[i][0] == '=')
            continue;
        for (int k = 0; k < old.n && !keep; k++)
            keep = envv_namelen(old.v[k]) == l && !_strnicmp(old.v[k], e->v[i], l);
        if (!keep) {
            char name[512];
            snprintf(name, sizeof name, "%.*s", (int)l, e->v[i]);
            _putenv_s(name, "");
        }
    }
    for (int i = 0; i < old.n; i++)
        if (old.v[i][0] != '=')
            _putenv(old.v[i]);
    envv_free(&old);
    return r;
}

static void exit_pipe(int ep[2]) { ep[0] = ep[1] = -1; }
#else
/* a pipe brings the 32-bit exit code of a child ntrun to its parent (see guest_exit) */
static void exit_pipe(int ep[2])
{
    if (pipe(ep) < 0) {
        ep[0] = ep[1] = -1;
        return;
    }
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFD, FD_CLOEXEC);
}

static void exit_pipe_child(const int ep[2]) /* in the child, before it starts the program */
{
    char b[16];

    if (ep[0] >= 0)
        close(ep[0]);
    if (ep[1] >= 0) {
        snprintf(b, sizeof b, "%d", ep[1]);
        setenv("NTRUN_EXITFD", b, 1);
    } else {
        unsetenv("NTRUN_EXITFD");
    }
}

static u32 exit_pipe_code(int fd, int st) /* in the parent, when the child has ended */
{
    u32 code;
    int ok = fd >= 0 && read(fd, &code, 4) == 4;

    if (fd >= 0)
        close(fd);
    if (ok)
        return code;
    return WIFEXITED(st) ? (u32)WEXITSTATUS(st) : STATUS_ACCESS_VIOLATION;
}
#endif

int find_program(const char *name, char *out, size_t n)
{
    char g[PATH_MAX], tmp[PATH_MAX];
    const char *base, *p;
    int hasext;

    snprintf(g, sizeof g, "%s", name);
    base = strrchr(g, '\\');
    if (!base)
        base = strrchr(g, '/');
    hasext = strchr(base ? base : g, '.') != NULL;
    if (!hasext)
        strcat(g, ".exe");
    if (base || (g[0] && g[1] == ':')) {
        to_host(g, out, n);
        return access(out, R_OK) == 0;
    }
    if (find_file(exe_hdir, g, out, n))
        return 1;
    snprintf(out, n, "%s", g);
    if (fix_case(out))
        return 1;
    if ((p = env_get("PATH"))) {
        while (*p) {
            size_t l = strcspn(p, ";");
            char gdir[PATH_MAX];
            snprintf(gdir, sizeof gdir, "%.*s", (int)l, p);
            if (gdir[0]) {
                to_host(gdir, tmp, sizeof tmp);
                if (find_file(tmp, g, out, n))
                    return 1;
            }
            p += l;
            if (*p)
                p++;
        }
    }
    return 0;
}

API(CreateProcessA)
{
    char prog[PATH_MAX], hp[PATH_MAX], *cmd = a[1] ? gstr(a[1]) : NULL;
    u32 si = a[8], pi = a[9], hp_, ht;
    pid_t pid;
    int ep[2];
#ifdef _WIN32
    intptr_t proc_h = 0;
#endif
    u32 sh[3] = { std_h[0], std_h[1], std_h[2] };

    if (a[0]) {
        snprintf(prog, sizeof prog, "%s", gstr(a[0]));
    } else if (!cmd) {
        seterr(87);
        return 0;
    } else {
        const char *s = cmd;
        size_t o = 0;
        while (*s == ' ')
            s++;
        if (*s == '"') {
            for (s++; *s && *s != '"' && o < sizeof prog - 1; s++)
                prog[o++] = *s;
        } else {
            for (; *s && *s != ' ' && *s != '\t' && o < sizeof prog - 1; s++)
                prog[o++] = *s;
        }
        prog[o] = 0;
    }
    if (!find_program(prog, hp, sizeof hp)) {
        if (!cmd_is_name(prog)) {
            seterr(2);
            return 0;
        }
        snprintf(hp, sizeof hp, "cmd.exe"); /* ntrun's own small command processor */
    }
    if (si && (r32(si + 44) & 0x100)) {
        sh[0] = r32(si + 56);
        sh[1] = r32(si + 60);
        sh[2] = r32(si + 64);
    }
    fflush(stdout);
    fflush(stderr);
    exit_pipe(ep);
#ifdef _WIN32
    {
        struct envv ev;
        char buf[64], cwd0[PATH_MAX] = "", *map;
        size_t o = 0;
        intptr_t proc;

        envv_init(&ev);
        /* the child's environment: the guest's, as it is */
        if (a[6]) {
            const char *e = gstr(a[6]);
            for (int i = 0; i < ngenv; i++)
                if (genv[i][0] != '=')
                    envv_unset(&ev, genv[i], envv_namelen(genv[i]));
            while (*e) {
                envv_set(&ev, e);
                e += strlen(e) + 1;
            }
        } else {
            for (int i = 0; i < ngenv; i++)
                if (genv[i][0] != '=')
                    envv_set(&ev, genv[i]);
        }
        envv_set(&ev, "NTRUN_CHILD=1");
        map = malloc(strlen(cmd ? cmd : prog) + 16);
        sprintf(map, "NTRUN_CMDLINE=%s", cmd ? cmd : prog);
        envv_set(&ev, map);
        free(map);
        snprintf(buf, sizeof buf, "NTRUN_STD=%u,%u,%u", sh[0], sh[1], sh[2]);
        envv_set(&ev, buf);
        map = malloc((size_t)NHND * 24 + 16);
        o = (size_t)sprintf(map, "NTRUN_HMAP=");
        for (u32 i = 0; i < NHND; i++)
            if (H[i].type == HT_FILE)
                o += (size_t)sprintf(map + o, "%u:%d,", HBASE + i * 4, H[i].fd);
        envv_set(&ev, map);
        free(map);
        if (si && r16(si + 50) && r32(si + 52)) {
            u32 n = r16(si + 50), p = r32(si + 52);
            char *hex = malloc(n * 2 + 16);
            o = (size_t)sprintf(hex, "NTRUN_RES2=");
            for (u32 i = 0; i < n; i++)
                o += (size_t)sprintf(hex + o, "%02x", r8(p + i));
            envv_set(&ev, hex);
            free(hex);
        } else {
            envv_unset(&ev, "NTRUN_RES2", 10);
        }
        if (a[7]) {
            char cwd[PATH_MAX];
            to_host(gstr(a[7]), cwd, sizeof cwd);
            getcwd(cwd0, sizeof cwd0);
            if (chdir(cwd) < 0) {
                envv_free(&ev);
                seterr(267); /* ERROR_DIRECTORY */
                return 0;
            }
        }
        proc = spawn_ntrun(_P_NOWAIT, hp, &ev);
        if (cwd0[0])
            chdir(cwd0);
        envv_free(&ev);
        if (proc == -1) {
            seterr(errno == ENOENT ? 2 : 8);
            return 0;
        }
        pid = (pid_t)GetProcessId((HANDLE)proc);
        proc_h = proc;
    }
#else
    pid = fork();
    if (pid < 0) {
        seterr(8);
        return 0;
    }
    if (pid == 0) {
        char buf[64], *map, *argv[3];
        size_t o = 0;

        exit_pipe_child(ep);

        if (a[7]) {
            char cwd[PATH_MAX];
            to_host(gstr(a[7]), cwd, sizeof cwd);
            if (chdir(cwd) < 0)
                _exit(126);
        }
        /* the child's environment: the guest's, as it is */
        if (a[6]) {
            const char *e = gstr(a[6]);
            for (int i = 0; i < ngenv; i++) {
                char *eq = strchr(genv[i], '=');
                if (eq && eq != genv[i]) {
                    *eq = 0;
                    unsetenv(genv[i]);
                }
            }
            while (*e) {
                putenv(strdup(e));
                e += strlen(e) + 1;
            }
        } else {
            for (int i = 0; i < ngenv; i++)
                if (genv[i][0] != '=')
                    putenv(genv[i]);
        }
        setenv("NTRUN_CHILD", "1", 1);
        setenv("NTRUN_CMDLINE", cmd ? cmd : prog, 1);
        snprintf(buf, sizeof buf, "%u,%u,%u", sh[0], sh[1], sh[2]);
        setenv("NTRUN_STD", buf, 1);
        map = malloc(NHND * 24);
        map[0] = 0;
        for (u32 i = 0; i < NHND; i++)
            if (H[i].type == HT_FILE)
                o += (size_t)sprintf(map + o, "%u:%d,", HBASE + i * 4, H[i].fd);
        setenv("NTRUN_HMAP", map, 1);
        if (si && r16(si + 50) && r32(si + 52)) {
            u32 n = r16(si + 50), p = r32(si + 52);
            char *hex = malloc(n * 2 + 1);
            for (u32 i = 0; i < n; i++)
                sprintf(hex + i * 2, "%02x", r8(p + i));
            setenv("NTRUN_RES2", hex, 1);
        } else {
            unsetenv("NTRUN_RES2");
        }
        argv[0] = self_path;
        argv[1] = hp;
        argv[2] = NULL;
        execv(self_path, argv);
        fprintf(stderr, "ntrun: cannot run %s: %s\n", self_path, strerror(errno));
        _exit(127);
    }
#endif
    if (ep[1] >= 0)
        close(ep[1]);
    hp_ = hnew(HT_PROC);
    hget(hp_)->pid = pid;
#ifdef _WIN32
    hget(hp_)->proc = proc_h;
#endif
    hget(hp_)->fd = ep[0];
    ht = hnew(HT_THREAD);
    hget(ht)->thr = -1; /* a thread of the other process */
    w32(pi, hp_);
    w32(pi + 4, ht);
    w32(pi + 8, (u32)pid);
    w32(pi + 12, (u32)pid + 1);
    return 1;
}

/* a UTF-16 string of the program as a new ANSI string in its memory (0 gives 0) */
static u32 wide_dup(u32 w)
{
    u32 n = 0, p;

    if (!w)
        return 0;
    while (r16(w + 2 * n))
        n++;
    p = galloc(n + 1);
    for (u32 k = 0; k <= n; k++)
        w8(p + k, r16(w + 2 * k) < 256 ? (u8)r16(w + 2 * k) : '?');
    return p;
}

API(CreateProcessW)
{
    u32 b[16], r;

    memcpy(b, a, sizeof b);
    b[0] = wide_dup(a[0]);
    b[1] = wide_dup(a[1]);
    b[7] = wide_dup(a[7]);
    if (a[6] && (a[5] & 0x400)) { /* CREATE_UNICODE_ENVIRONMENT: the block ends with an empty string */
        u32 n = 0;
        while (r16(a[6] + 2 * n) || r16(a[6] + 2 * n + 2))
            n++;
        n += 2;
        b[6] = galloc(n);
        for (u32 k = 0; k < n; k++)
            w8(b[6] + k, r16(a[6] + 2 * k) < 256 ? (u8)r16(a[6] + 2 * k) : '?');
    }
    r = k_CreateProcessA(b);
    if (b[6] != a[6])
        gfree(b[6]);
    if (b[0])
        gfree(b[0]);
    if (b[1])
        gfree(b[1]);
    if (b[7])
        gfree(b[7]);
    return r;
}

static void reap(struct hnd *x, int block)
{
    int st;
    pid_t r;

    if (x->done)
        return;
#ifdef _WIN32
    {
        DWORD code = 1;
        if (WaitForSingleObject((HANDLE)x->proc, block ? INFINITE : 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess((HANDLE)x->proc, &code);
            x->done = 1;
            x->status = (int)code;
            CloseHandle((HANDLE)x->proc);
        }
        (void)st;
        (void)r;
    }
#else
    do
        r = waitpid(x->pid, &st, block ? 0 : WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == x->pid) {
        x->done = 1;
        x->status = (int)exit_pipe_code(x->fd, st);
        x->fd = -1;
    } else if (r < 0) {
        x->done = 1;
        x->status = 1;
    }
#endif
}

/* Is the thing behind a handle signaled? TAKE: a wait ends with it, so it changes:
   an event that does not stay set goes off, a mutex gets its thread, a semaphore counts down. */
static int obj_ready(struct hnd *x, int take)
{
    switch (x->type) {
    case HT_PROC:
        reap(x, 0);
        return x->done;
    case HT_THREAD:
        return x->thr < 0 || threads[x->thr].done;
    case HT_SYNC:
        if (x->kind == SY_EVENT) {
            if (!x->count)
                return 0;
            if (take && !x->manual)
                x->count = 0;
        } else if (x->kind == SY_MUTEX) {
            if (x->owner && x->owner != cur->id && thread_alive(x->owner))
                return 0;
            if (take) {
                if (x->owner != cur->id)
                    x->count = 0;
                x->owner = cur->id;
                x->count++;
            }
        } else if (x->kind == SY_SEM) {
            if (x->count <= 0)
                return 0;
            if (take)
                x->count--;
        }
        return 1;
    }
    return 1; /* a file and the like */
}

/* wait for one (or, with ALL, for all) of N handles, at most TIMEOUT milliseconds */
static u32 wait_for(u32 n, const u32 *h, int all, u32 timeout)
{
    struct hnd *x[64];
    struct timeval t0, t1;
    u32 last = 0;

    if (!n || n > 64) {
        seterr(87);
        return INVALID;
    }
    for (u32 i = 0; i < n; i++) {
        x[i] = hget(h[i]);
        if (!x[i]) {
            seterr(6);
            return INVALID;
        }
    }
    gettimeofday(&t0, NULL);
    for (int k = 0;; k++) {
        if (all) {
            /* the result is the number of the one that ended the wait (as Windows NT gives it) */
            u32 ok = 1, was = last;
            for (u32 i = 0; i < n; i++)
                if (!obj_ready(x[i], 0)) {
                    ok = 0;
                    last = i;
                }
            if (ok) {
                for (u32 i = 0; i < n; i++)
                    obj_ready(x[i], 1);
                return was;
            }
        } else {
            for (u32 i = 0; i < n; i++)
                if (obj_ready(x[i], 1))
                    return i;
        }
        if (timeout != INVALID) {
            gettimeofday(&t1, NULL);
            if ((u64)(t1.tv_sec - t0.tv_sec) * 1000 + (u64)(t1.tv_usec / 1000) - (u64)(t0.tv_usec / 1000) >= timeout)
                return 0x102; /* the time is over */
        }
        thread_wait(k);
    }
}

API(WaitForSingleObject)
{
    struct hnd *x = hget(a[0]);

    if (x && x->type == HT_PROC && a[1] == INVALID) {
        /* the common case, a program waits for the one it started: no need to look again and again */
        struct thread *me = cur;
        int m = multi;
        if (m)
            cpu_give();
        reap(x, 1);
        if (m)
            cpu_take(me);
        return 0;
    }
    return wait_for(1, &a[0], 0, a[1]);
}

API(WaitForMultipleObjects) /* (count, handles, all, timeout) */
{
    u32 h[64];

    for (u32 i = 0; i < a[0] && i < 64; i++)
        h[i] = r32(a[1] + 4 * i);
    return wait_for(a[0], h, a[2] != 0, a[3]);
}

API(GetExitCodeProcess)
{
    struct hnd *x = hget(a[0]);
    if (!x || x->type != HT_PROC) {
        seterr(6);
        return 0;
    }
    reap(x, 0);
    w32(a[1], x->done ? (u32)x->status : 259);
    return 1;
}

API(DuplicateHandle)
{
    struct hnd *x = hget(a[1]);
    u32 h;
    if (a[1] == 0xfffffffe) { /* GetCurrentThread(): a handle that other threads can use */
        h = hnew(HT_THREAD);
        hget(h)->thr = (int)(cur - threads);
        if (a[3])
            w32(a[3], h);
        return 1;
    }
    if (!x) {
        seterr(6);
        return 0;
    }
    if (x->type == HT_FILE) {
        int fd = dup(x->fd);
        if (fd < 0) {
            seterr(werr(errno));
            return 0;
        }
        h = hfile(fd);
    } else {
        struct hnd copy = *x;
        h = hnew(x->type);
        copy.path = NULL;
        copy.names = NULL;
        copy.dir = NULL;
        *hget(h) = copy;
    }
    if (a[3])
        w32(a[3], h);
    if (a[6] & 1)
        hclose(hget(a[1]));
    return 1;
}

API(CreatePipe)
{
    int fds[2];
    if (pipe(fds) < 0) {
        seterr(werr(errno));
        return 0;
    }
    w32(a[0], hfile(fds[0]));
    w32(a[1], hfile(fds[1]));
    return 1;
}

API(PeekNamedPipe)
{
    int fd = hfd(a[0]), avail = 0;
    if (fd < 0) {
        seterr(6);
        return 0;
    }
    ioctl(fd, FIONREAD, &avail);
    if (a[3]) w32(a[3], 0);
    if (a[4]) w32(a[4], (u32)avail);
    if (a[5]) w32(a[5], 0);
    return 1;
}

/* small things */
API(GetProcessHeap) { UNUSED; static u32 h; if (!h) h = galloc(64); return h; }
API(LocalAlloc) { u32 p = galloc(a[1]); if (p && (a[0] & 0x40)) memset(G + p, 0, a[1]); return p; }
API(LocalFree) { gfree(a[0]); return 0; }
API(GlobalAlloc) { return k_LocalAlloc(a); }
API(LocalReAlloc) { return a[0] ? grealloc(a[0], a[1], (a[2] & 0x40) != 0, 0) : 0; }
API(GlobalReAlloc) { return k_LocalReAlloc(a); }
API(LocalSize) { return a[0] ? gsize(a[0]) : 0; }
API(GlobalSize) { return k_LocalSize(a); }
API(LocalLock) { return a[0]; }
API(LocalUnlock) { UNUSED; return 1; }
API(IsCharLowerA) { u32 c = a[0] & 0xff; return mapchar(c, 0x200) != c; }
API(IsCharUpperA) { u32 c = a[0] & 0xff; return mapchar(c, 0x100) != c; }
API(IsCharAlphaA) { u32 c = a[0] & 0xff; return mapchar(c, 0x200) != c || mapchar(c, 0x100) != c; }
API(IsCharAlphaNumericA) { u32 c = a[0] & 0xff; return (c >= 0x30 && c <= 0x39) || k_IsCharAlphaA(a); }
API(RtlCopyMemory) { memmove(G + a[0], G + a[1], a[2]); return 0; }
API(GlobalFree) { gfree(a[0]); return 0; }
API(CharToOemA) { if (a[0] != a[1]) memmove(G + a[1], G + a[0], strlen(gstr(a[0])) + 1); return 1; }
API(OemToCharA) { return k_CharToOemA(a); }
API(CharNextA) { return r8(a[0]) ? a[0] + 1 : a[0]; }
API(CharUpperBuffW) { for (u32 i = 0; i < a[1]; i++) w16(a[0] + i * 2, (u16)mapchar(r16(a[0] + i * 2), 0x200)); return a[1]; }
API(SetFileApisToOEM) { UNUSED; return 0; }
API(SetFileApisToANSI) { UNUSED; return 0; }
API(AreFileApisANSI) { UNUSED; return 1; }
API(GetConsoleCP) { UNUSED; return 437; }
API(GetConsoleOutputCP) { UNUSED; return 437; }
API(IsDBCSLeadByte) { UNUSED; return 0; }
API(IsDBCSLeadByteEx) { UNUSED; return 0; }
API(SendMessageA) { UNUSED; return 0; }
API(GetNumberOfConsoleInputEvents) { if (a[1]) w32(a[1], 0); return 1; }
API(PeekConsoleInputA) { if (a[3]) w32(a[3], 0); return 1; }
API(ReadConsoleInputA) { if (a[3]) w32(a[3], 0); seterr(6); return 0; }
API(FatalAppExitA) { fflush(stdout); fprintf(stderr, "%s\n", gstr(a[1])); exit(3); }
API(MessageBoxA) { fflush(stdout); fprintf(stderr, "%s: %s\n", a[2] ? gstr(a[2]) : "Error", a[1] ? gstr(a[1]) : ""); return 1; }

API(WriteConsoleW)
{
    u32 n = a[2];
    char *b = malloc(n + 1);
    int fd = hfd(a[0]);
    for (u32 i = 0; i < n; i++) {
        u16 c = r16(a[1] + i * 2);
        b[i] = c < 256 ? (char)c : '?';
    }
    if (fd >= 0 && write(fd, b, n) < 0)
        n = 0;
    free(b);
    if (a[3])
        w32(a[3], n);
    return fd >= 0;
}

API(DosDateTimeToFileTime)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (int)((a[0] >> 9) & 0x7f) + 80;
    tm.tm_mon = (int)((a[0] >> 5) & 15) - 1;
    tm.tm_mday = (int)(a[0] & 31);
    tm.tm_hour = (int)((a[1] >> 11) & 31);
    tm.tm_min = (int)((a[1] >> 5) & 63);
    tm.tm_sec = (int)(a[1] & 31) * 2;
    /* a date of 0 is not a date: nmake gives it for a file that is not there */
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59) {
        seterr(87);
        return 0;
    }
    w64(a[2], unix_to_ft(timegm(&tm), 0));
    return 1;
}

API(FileTimeToDosDateTime)
{
    u64 ft = r64(a[0]);
    time_t t = ft_to_unix(ft);
    struct tm tm;
    if (ft < 119600064000000000ull) { /* before 1980 */
        seterr(87);
        return 0;
    }
    gmtime_r(&t, &tm);
    if (tm.tm_year > 207) {
        seterr(87);
        return 0;
    }
    w16(a[1], (u16)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    w16(a[2], (u16)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    return 1;
}

API(CopyFileA)
{
    char h1[PATH_MAX], h2[PATH_MAX], buf[65536];
    int in, out;
    ssize_t n;
    to_host(gstr(a[0]), h1, sizeof h1);
    to_host(gstr(a[1]), h2, sizeof h2);
    in = open(h1, O_RDONLY);
    if (in < 0) {
        seterr(werr(errno));
        return 0;
    }
    out = open(h2, O_WRONLY | O_CREAT | (a[2] ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) {
        seterr(errno == EEXIST ? 80 : werr(errno));
        close(in);
        return 0;
    }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n)
            break;
    close(in);
    close(out);
    return 1;
}

API(GetTempPathA)
{
    const char *t = env_get("TMP");
    char g[PATH_MAX];
    u32 n;
    if (!t)
        t = env_get("TEMP");
    if (t) {
        snprintf(g, sizeof g, "%s", t);
    } else {
        to_guest("/tmp", g, sizeof g);
    }
    n = (u32)strlen(g);
    if (n && g[n - 1] != '\\') {
        g[n++] = '\\';
        g[n] = 0;
    }
    if (n + 1 > a[0])
        return n + 1;
    memcpy(G + a[1], g, n + 1);
    return n;
}

API(GetTempFileNameA)
{
    static u32 seq;
    char g[PATH_MAX], hp[PATH_MAX];
    u32 uniq = a[2];
    for (int tries = 0; tries < 65536; tries++) {
        u32 u = uniq ? uniq : (((u32)getpid() + seq++) & 0xffff);
        int fd;
        if (!u)
            continue;
        snprintf(g, sizeof g, "%s%s%.3s%x.tmp", gstr(a[0]), (r8(a[0]) && gstr(a[0])[strlen(gstr(a[0])) - 1] != '\\') ? "\\" : "",
            a[1] ? gstr(a[1]) : "", u);
        if (uniq) {
            strcpy(gstr(a[3]), g);
            return u;
        }
        to_host(g, hp, sizeof hp);
        fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            close(fd);
            strcpy(gstr(a[3]), g);
            return u;
        }
        if (errno != EEXIST)
            break;
    }
    seterr(5);
    return 0;
}

/* more small things: strings, versions, files in the old style, the registry, the rest */
API(lstrlenA) { return a[0] ? (u32)strlen(gstr(a[0])) : 0; }
API(lstrlenW) { u32 n = 0; if (a[0]) while (r16(a[0] + n * 2)) n++; return n; }
API(lstrcpyA) { if (!a[0] || !a[1]) return 0; memmove(G + a[0], G + a[1], strlen(gstr(a[1])) + 1); return a[0]; }
API(lstrcatA)
{
    char *d;
    const char *s;

    if (!a[0] || !a[1])
        return 0;
    d = gstr(a[0]);
    s = gstr(a[1]);
    memmove(d + strlen(d), s, strlen(s) + 1); /* the two can be in the same buffer */
    return a[0];
}
API(lstrcmpA) { int c = strcmp(gstr(a[0]), gstr(a[1])); return (u32)(c < 0 ? -1 : c > 0); }
API(lstrcmpiA) { int c = strcasecmp(gstr(a[0]), gstr(a[1])); return (u32)(c < 0 ? -1 : c > 0); }
API(lstrcpynA)
{
    u32 n = 0;
    if (!a[0] || !a[1] || !a[2])
        return 0;
    while (n + 1 < a[2] && r8(a[1] + n)) {
        w8(a[0] + n, r8(a[1] + n));
        n++;
    }
    w8(a[0] + n, 0);
    return a[0];
}
API(CharPrevA) { return a[1] > a[0] ? a[1] - 1 : a[0]; }
API(CharUpperA)
{
    if (a[0] < 0x10000)
        return mapchar(a[0], 0x200);
    for (u32 p = a[0]; r8(p); p++)
        w8(p, (u8)mapchar(r8(p), 0x200));
    return a[0];
}
API(CharLowerA)
{
    if (a[0] < 0x10000)
        return mapchar(a[0], 0x100);
    for (u32 p = a[0]; r8(p); p++)
        w8(p, (u8)mapchar(r8(p), 0x100));
    return a[0];
}
API(CharUpperBuffA) { for (u32 i = 0; i < a[1]; i++) w8(a[0] + i, (u8)mapchar(r8(a[0] + i), 0x200)); return a[1]; }
API(CharLowerBuffA) { for (u32 i = 0; i < a[1]; i++) w8(a[0] + i, (u8)mapchar(r8(a[0] + i), 0x100)); return a[1]; }
API(IsBadStringPtrA) { return !guest_ok(a[0], 1); }
API(OutputDebugStringA) { UNUSED; return 0; }
API(SendMessageW) { UNUSED; return 0; }
API(GlobalLock) { return a[0]; }
API(GlobalUnlock) { UNUSED; return 1; }
API(GetSystemDefaultLCID) { UNUSED; return 0x409; }
API(GetSystemDefaultLangID) { UNUSED; return 0x409; }
API(GetUserDefaultLangID) { UNUSED; return 0x409; }
API(GetThreadLocale) { UNUSED; return 0x409; }
API(EnumSystemLocalesA) { UNUSED; return 1; }
API(SetConsoleTitleA) { UNUSED; return 1; }
API(SetThreadPriority) { UNUSED; return 1; }
API(GetThreadPriority) { UNUSED; return 0; }
API(SetPriorityClass) { UNUSED; return 1; }
API(FlushViewOfFile) { UNUSED; views_sync(); return 1; }
API(FlushInstructionCache) { UNUSED; return 1; }
API(GetProcessAffinityMask) { if (a[1]) w32(a[1], 1); if (a[2]) w32(a[2], 1); return 1; }

API(GetVersionExA)
{
    u32 p = a[0];
    if (r32(p) < 148) {
        seterr(122);
        return 0;
    }
    w32(p + 4, 4);    /* Windows NT 4.0, build 1381 */
    w32(p + 8, 0);
    w32(p + 12, 1381);
    w32(p + 16, 2);   /* VER_PLATFORM_WIN32_NT */
    strcpy(gstr(p + 20), "Service Pack 3");
    return 1;
}

API(GetSystemTimeAsFileTime)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    w64(a[0], unix_to_ft(tv.tv_sec, (long)tv.tv_usec * 1000));
    return 0;
}

API(QueryPerformanceFrequency) { w64(a[0], 1000000); return 1; }
API(QueryPerformanceCounter)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    w64(a[0], (u64)tv.tv_sec * 1000000 + (u64)tv.tv_usec);
    return 1;
}

API(GlobalMemoryStatus)
{
    u32 p = a[0];
    w32(p, 32);
    w32(p + 4, 25);             /* memory load, percent */
    w32(p + 8, 0x08000000);     /* 128 MB of memory */
    w32(p + 12, 0x06000000);
    w32(p + 16, 0x20000000);    /* page file */
    w32(p + 20, 0x1c000000);
    w32(p + 24, 0x7ffe0000);    /* address space */
    w32(p + 28, 0x70000000);
    return 0;
}

API(GetEnvironmentVariableA)
{
    const char *v = env_get(gstr(a[0]));
    u32 n;
    if (!v) {
        seterr(203);
        return 0;
    }
    n = (u32)strlen(v);
    if (n + 1 > a[2])
        return n + 1;
    memcpy(G + a[1], v, n + 1);
    return n;
}

API(ExpandEnvironmentStringsA)
{
    const char *s = gstr(a[0]);
    char out[8192];
    size_t o = 0;
    while (*s && o < sizeof out - 1) {
        const char *e;
        if (*s == '%' && (e = strchr(s + 1, '%'))) {
            char name[256];
            const char *v;
            snprintf(name, sizeof name, "%.*s", (int)(e - s - 1), s + 1);
            if ((v = env_get(name))) {
                o += (size_t)snprintf(out + o, sizeof out - o, "%s", v);
                s = e + 1;
                continue;
            }
        }
        out[o++] = *s++;
    }
    if (o >= sizeof out)
        o = sizeof out - 1;
    out[o] = 0;
    if (o + 1 <= a[2])
        memcpy(G + a[1], out, o + 1);
    return (u32)o + 1;
}

API(GetComputerNameA)
{
    const char *n = "NTRUN";
    if (r32(a[1]) < strlen(n) + 1) {
        w32(a[1], (u32)strlen(n) + 1);
        seterr(111);
        return 0;
    }
    strcpy(gstr(a[0]), n);
    w32(a[1], (u32)strlen(n));
    return 1;
}

API(GetUserNameA)
{
    const char *n = "Administrator";
    if (r32(a[1]) < strlen(n) + 1) {
        w32(a[1], (u32)strlen(n) + 1);
        seterr(122);
        return 0;
    }
    strcpy(gstr(a[0]), n);
    w32(a[1], (u32)strlen(n) + 1);
    return 1;
}

API(GetShortPathNameA)
{
    u32 n = (u32)strlen(gstr(a[0]));
    if (n + 1 > a[2])
        return n + 1;
    memmove(G + a[1], G + a[0], n + 1);
    return n;
}

API(GetSystemDirectoryA)
{
    char d[32];
    u32 n = (u32)snprintf(d, sizeof d, "%c:\\windows\\system32", root_letter);
    if (n + 1 > a[1])
        return n + 1;
    memcpy(G + a[0], d, n + 1);
    return n;
}

API(GetWindowsDirectoryA)
{
    char d[32];
    u32 n = (u32)snprintf(d, sizeof d, "%c:\\windows", root_letter);
    if (n + 1 > a[1])
        return n + 1;
    memcpy(G + a[0], d, n + 1);
    return n;
}

/* SearchPath(path, file, extension, size, buffer, file part): the program's folder, here, the PATH */
API(SearchPathA)
{
    char name[PATH_MAX], hp[PATH_MAX], g[PATH_MAX], dirs[8192];
    const char *p;
    u32 n;
    int found = 0;

    snprintf(name, sizeof name, "%s", gstr(a[1]));
    if (a[2] && !strchr(name, '.'))
        strncat(name, gstr(a[2]), sizeof name - strlen(name) - 1);
    if (strchr(name, '\\') || strchr(name, '/') || (name[0] && name[1] == ':')) {
        to_host(name, hp, sizeof hp);
        found = access(hp, F_OK) == 0;
    } else {
        if (a[0]) {
            snprintf(dirs, sizeof dirs, "%s", gstr(a[0]));
        } else {
            char ed[PATH_MAX];
            to_guest(exe_hdir, ed, sizeof ed);
            snprintf(dirs, sizeof dirs, "%s;.;%s", ed, env_get("PATH") ? env_get("PATH") : "");
        }
        for (p = dirs; *p && !found;) {
            size_t l = strcspn(p, ";");
            char gd[PATH_MAX], hd[PATH_MAX];
            snprintf(gd, sizeof gd, "%.*s", (int)l, p);
            if (gd[0]) {
                to_host(gd, hd, sizeof hd);
                found = find_file(hd, name, hp, sizeof hp);
            }
            p += l;
            if (*p)
                p++;
        }
    }
    if (!found) {
        seterr(2);
        return 0;
    }
    to_guest(hp, g, sizeof g);
    n = (u32)strlen(g);
    if (n + 1 > a[3])
        return n + 1;
    memcpy(G + a[4], g, n + 1);
    if (a[5]) {
        char *bs = strrchr(g, '\\');
        w32(a[5], bs ? a[4] + (u32)(bs + 1 - g) : a[4]);
    }
    return n;
}

API(MoveFileExA)
{
    char h1[PATH_MAX], h2[PATH_MAX];
    struct stat st;
    if (!a[1]) /* delete at the next start of Windows */
        return 1;
    to_host(gstr(a[0]), h1, sizeof h1);
    to_host(gstr(a[1]), h2, sizeof h2);
    if (!(a[2] & 1) && stat(h2, &st) == 0) {
        seterr(183);
        return 0;
    }
    if (rename(h1, h2) < 0) {
        seterr(werr(errno));
        return 0;
    }
    return 1;
}

/* the file functions of 16-bit Windows: a file is a handle too */
API(_lopen)
{
    u32 b[7] = { a[0], (a[1] & 3) == 0 ? 0x80000000u : (a[1] & 3) == 1 ? 0x40000000u : 0xc0000000u, 3, 0, 3, 0x80, 0 };
    return k_CreateFileA(b);
}
API(_lcreat)
{
    u32 b[7] = { a[0], 0xc0000000u, 3, 0, 2, 0x80, 0 };
    return k_CreateFileA(b);
}
API(_lclose) { u32 b[1] = { a[0] }; return k_CloseHandle(b) ? 0 : INVALID; }
API(_lread)
{
    u32 n = galloc(4), b[5] = { a[0], a[1], a[2], n, 0 }, r = k_ReadFile(b) ? r32(n) : INVALID;
    gfree(n);
    return r;
}
API(_lwrite)
{
    u32 n = galloc(4), b[5] = { a[0], a[1], a[2], n, 0 }, r = k_WriteFile(b) ? r32(n) : INVALID;
    gfree(n);
    return r;
}
API(_llseek) { u32 b[4] = { a[0], a[1], 0, a[2] }; return k_SetFilePointer(b); }

/* OpenFile(name, OFSTRUCT, style) */
API(OpenFile)
{
    u32 st = a[2], of = a[1], h;
    char hp[PATH_MAX], g[PATH_MAX];

    to_host(gstr(a[0]), hp, sizeof hp);
    to_guest(hp, g, sizeof g);
    memset(G + of, 0, 136);
    w8(of, 136);
    snprintf(gstr(of + 8), 128, "%s", g);
    if (st & 0x4000) { /* OF_EXIST */
        if (access(hp, F_OK) != 0) {
            seterr(2);
            w16(of + 2, 2);
            return INVALID;
        }
        return 1;
    }
    if (st & 0x0200) { /* OF_DELETE */
        if (unlink(hp) < 0) {
            seterr(werr(errno));
            return INVALID;
        }
        return 1;
    }
    {
        u32 b[7] = { a[0], (st & 3) == 0 ? 0x80000000u : (st & 3) == 1 ? 0x40000000u : 0xc0000000u, 3, 0,
            (st & 0x1000) ? 2 : 3, 0x80, 0 };
        h = k_CreateFileA(b);
    }
    if (h == INVALID)
        w16(of + 2, (u16)last_error);
    return h;
}

/* reading and writing the memory of this process */
API(ReadProcessMemory)
{
    if (a[1] < 0x10000 || (u64)a[1] + a[3] > GSIZE) {
        seterr(299);
        return 0;
    }
    memmove(G + a[2], G + a[1], a[3]);
    if (a[4])
        w32(a[4], a[3]);
    return 1;
}
API(WriteProcessMemory)
{
    if (a[1] < 0x10000 || (u64)a[1] + a[3] > GSIZE) {
        seterr(299);
        return 0;
    }
    memmove(G + a[1], G + a[2], a[3]);
    if (a[4])
        w32(a[4], a[3]);
    return 1;
}

/* VirtualQuery: enough for a program that asks about its stack or an image */
API(VirtualQuery)
{
    u32 p = a[1], ps = cpu->pagesize, base = a[0] & ~(ps - 1), c = a[0] / CHUNK, abase, n;

    if (a[2] < 28 || a[0] >= GSIZE) {
        seterr(87);
        return 0;
    }
    for (abase = c; abase > 0 && (rmap[abase - 1] == 1) == (rmap[c] == 1); abase--)
        ;
    for (n = c; n < NCHUNK && (rmap[n] == 1) == (rmap[c] == 1); n++)
        ;
    w32(p, base);
    w32(p + 4, abase * CHUNK);
    w32(p + 8, 4);
    w32(p + 12, n * CHUNK - base);
    w32(p + 16, (rmap[c] == 1) ? 0x1000 : 0x10000); /* MEM_COMMIT or MEM_FREE */
    w32(p + 20, (rmap[c] == 1) ? 4 : 1);
    w32(p + 24, (rmap[c] == 1) ? 0x20000 : 0);
    return 28;
}

/* events */
API(CreateEventA) /* (security, it stays set, set from the start, name) */
{
    u32 h = hnew(HT_SYNC);
    hget(h)->kind = SY_EVENT;
    hget(h)->manual = a[1] != 0;
    hget(h)->count = a[2] != 0;
    return h;
}
API(SetEvent) { struct hnd *x = hget(a[0]); if (x) x->count = 1; return x != NULL; }
API(ResetEvent) { struct hnd *x = hget(a[0]); if (x) x->count = 0; return x != NULL; }
API(PulseEvent)
{
    struct hnd *x = hget(a[0]);
    if (!x)
        return 0;
    x->count = 1; /* the threads that wait now see it */
    thread_yield();
    x->count = 0;
    return 1;
}

/* ---- threads ---- */

static u32 exe_gp, peb_g, stack_default = 0x100000;

/* DllMain of each DLL for a thread that starts (2) or ends (3) */
static void dll_thread_calls(u32 reason)
{
    for (int i = nmods - 1; i >= 0; i--) {
        struct module *m = &mods[i];
        if (!m->builtin && m->is_dll && m->entry && m->inited) {
            u32 b[3] = { m->base, reason, 0 };
            guest_call(m->entry, 3, b);
        }
    }
}

/* The end of a thread; T has the CPU. The program ends with its last thread. */
static void thread_finish(struct thread *t, u32 code)
{
    if (!t->killed && multi)
        dll_thread_calls(3);
    t->exit_code = code;
    t->done = 1;
    if (--nthreads <= 0)
        guest_exit(code);
    cpu_free();
    pthread_exit(NULL);
    __builtin_unreachable();
}

static void *thread_host(void *arg)
{
    struct thread *t = arg;

    cpu_take(t);
    dll_thread_calls(2);
    thread_finish(t, guest_call(t->start, 1, &t->param));
}

static struct thread *thread_of(u32 h)
{
    struct hnd *x;

    if (h == 0xfffffffe) /* GetCurrentThread() */
        return cur;
    x = hget(h);
    return x && x->type == HT_THREAD && x->thr >= 0 ? &threads[x->thr] : NULL;
}

API(CreateThread) /* (security, stack size, start function, its argument, flags, &id) */
{
    struct thread *t = NULL;
    u32 size, stack, tebn, h;
    pthread_attr_t attr;
    void *save;

    for (int i = 1; i < NTHREADS; i++)
        if (!threads[i].used) {
            t = &threads[i];
            break;
        }
    size = a[1] ? (a[1] + CHUNK - 1) & ~(CHUNK - 1) : stack_default;
    stack = t ? region_alloc(0, size) : 0;
    tebn = stack ? region_alloc(0, CHUNK) : 0;
    if (!tebn) {
        seterr(8);
        return 0;
    }
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->id = threads[0].id + 4 * (u32)(t - threads);
    t->teb = tebn;
    t->start = a[2];
    t->param = a[3];
    t->suspend = (a[4] & 4) != 0; /* CREATE_SUSPENDED */
    w32(tebn + 4, stack + size);
    w32(tebn + 8, stack);
    w32(tebn + 0x18, tebn);
    w32(tebn + 0x20, (u32)getpid());
    w32(tebn + 0x24, t->id);
    w32(tebn + 0x30, peb_g);
    tls_thread(tebn);
    /* its registers at its start: made in the CPU, then the CPU gets back those of this thread */
    t->regs = calloc(1, cpu->state_size);
    save = malloc(cpu->state_size);
    cpu->state_save(save);
    cpu->init(stack + size, tebn, exe_gp);
    cpu->state_save(t->regs);
    cpu->state_load(save, teb);
    free(save);

    h = hnew(HT_THREAD);
    hget(h)->thr = (int)(t - threads);
    if (a[5])
        w32(a[5], t->id);
    multi = 1;
    nthreads++;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &attr, thread_host, t) != 0) {
        nthreads--;
        t->used = 0;
        hclose(hget(h));
        seterr(8);
        return 0;
    }
    return h;
}

API(ExitThread) { thread_finish(cur, a[0]); }

API(ResumeThread)
{
    struct thread *t = thread_of(a[0]);
    u32 n;

    if (!t) {
        seterr(6);
        return INVALID;
    }
    n = (u32)t->suspend;
    if (t->suspend)
        t->suspend--;
    return n;
}

API(SuspendThread)
{
    struct thread *t = thread_of(a[0]);
    u32 n;

    if (!t) {
        seterr(6);
        return INVALID;
    }
    n = (u32)t->suspend++;
    if (t == cur)
        thread_yield(); /* it goes on when another thread resumes it */
    return n;
}

API(GetExitCodeThread)
{
    struct thread *t = thread_of(a[0]);

    if (a[1])
        w32(a[1], t && t->done ? t->exit_code : 259); /* 259: it runs still */
    return 1;
}

API(TerminateThread)
{
    struct thread *t = thread_of(a[0]);

    if (!t) {
        seterr(6);
        return 0;
    }
    if (t->done)
        return 1;
    t->killed = 1;
    t->exit_code = a[1];
    if (t == cur)
        thread_finish(t, a[1]);
    return 1; /* it ends when the CPU comes to it */
}

/* interlocked functions (KERNEL32 has them as functions on the RISC CPUs) */
API(InterlockedIncrement) { u32 v = r32(a[0]) + 1; w32(a[0], v); return v; }
API(InterlockedDecrement) { u32 v = r32(a[0]) - 1; w32(a[0], v); return v; }
API(InterlockedExchange) { u32 v = r32(a[0]); w32(a[0], a[1]); return v; }

/* is the text UTF-16? Only when it starts with the byte order mark */
API(IsTextUnicode)
{
    int u = a[1] >= 2 && r16(a[0]) == 0xfeff;
    if (a[2])
        w32(a[2], u ? 2 : 0);
    return (u32)u;
}

/* the registry: nothing is there */
API(RegOpenKeyA) { UNUSED; return 2; }
API(RegOpenKeyExA) { UNUSED; return 2; }
API(RegCreateKeyA) { UNUSED; return 5; }
API(RegCreateKeyExA) { UNUSED; return 5; }
API(RegQueryValueA) { UNUSED; return 2; }
API(RegQueryValueExA) { UNUSED; return 2; }
API(RegSetValueExA) { UNUSED; return 5; }
API(RegDeleteValueA) { UNUSED; return 2; }
API(RegEnumKeyA) { UNUSED; return 259; }
API(RegCloseKey) { UNUSED; return 0; }

/* WIN.INI and other .ini files: the default answer */
API(GetProfileStringA)
{
    const char *d = a[2] ? gstr(a[2]) : "";
    u32 n = (u32)strlen(d);
    if (!a[4])
        return 0;
    if (n > a[4] - 1)
        n = a[4] - 1;
    memmove(G + a[3], d, n);
    w8(a[3] + n, 0);
    return n;
}
API(GetPrivateProfileStringA) { return k_GetProfileStringA(a); }
API(GetProfileIntA) { return a[2]; }
API(GetPrivateProfileIntA) { return a[2]; }
API(WriteProfileStringA) { UNUSED; return 1; }
API(WritePrivateProfileStringA) { UNUSED; return 1; }

/* RPCRT4: uuidgen needs these three */
API(UuidCreate)
{
#ifdef _WIN32
    /* rand() has 15 bits: take the bytes from rand_s */
    for (int i = 0; i < 16; i++) {
        unsigned int v;
        if (rand_s(&v) != 0)
            v = (unsigned)rand();
        w8(a[0] + (u32)i, (u8)(v >> 8));
    }
#else
    static int seeded;
    if (!seeded) {
        srandom((unsigned)(time(NULL) ^ getpid()));
        seeded = 1;
    }
    for (int i = 0; i < 16; i++)
        w8(a[0] + (u32)i, (u8)(random() >> 8));
#endif
    w8(a[0] + 7, (u8)((r8(a[0] + 7) & 0x0f) | 0x40)); /* version 4, in the little-endian third field */
    w8(a[0] + 8, (u8)((r8(a[0] + 8) & 0x3f) | 0x80));
    return 0;
}
API(UuidToStringA)
{
    char s[40];
    u32 u = a[0];
    snprintf(s, sizeof s, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", r32(u), r16(u + 4), r16(u + 6), r8(u + 8),
        r8(u + 9), r8(u + 10), r8(u + 11), r8(u + 12), r8(u + 13), r8(u + 14), r8(u + 15));
    w32(a[1], gdup(s, 37));
    return 0;
}
API(RpcStringFreeA) { gfree(r32(a[0])); w32(a[0], 0); return 0; }

/* OLE32: the C++ compilers from Visual C++ 5.0 on read the text of __declspec(uuid("...")) with
   this: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" in Unicode becomes the 16 bytes of a GUID. */
API(CLSIDFromString)
{
    static const char form[] = "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}";
    static const u8 place[16] = { 7, 5, 3, 1, 12, 10, 17, 15, 20, 22, 25, 27, 29, 31, 33, 35 }; /* of each byte */
    char s[39];

    if (!a[0]) {
        memset(G + a[1], 0, 16);
        return 0;
    }
    for (u32 i = 0; i < 39; i++) {
        u16 c = r16(a[0] + i * 2);
        if (form[i] == 'x' ? c > 127 || !isxdigit(c) : c != (u8)form[i])
            return 0x800401f3; /* CO_E_CLASSSTRING */
        s[i] = (char)c;
    }
    for (u32 i = 0; i < 16; i++) {
        char h[3] = { s[place[i]], s[place[i] + 1], 0 };
        w8(a[1] + i, (u8)strtoul(h, NULL, 16));
    }
    return 0;
}

/* NTDLL: the counted strings of the native API */
API(RtlInitString)
{
    u32 n = a[1] ? (u32)strlen(gstr(a[1])) : 0;
    w16(a[0], (u16)n);
    w16(a[0] + 2, (u16)(a[1] ? n + 1 : 0));
    w32(a[0] + 4, a[1]);
    return 0;
}
API(RtlInitAnsiString) { return k_RtlInitString(a); }
API(RtlInitUnicodeString)
{
    u32 n = 0;
    if (a[1])
        while (r16(a[1] + n * 2))
            n++;
    w16(a[0], (u16)(n * 2));
    w16(a[0] + 2, (u16)(a[1] ? n * 2 + 2 : 0));
    w32(a[0] + 4, a[1]);
    return 0;
}
API(RtlAnsiStringToUnicodeString) /* (unicode destination, ansi source, allocate) */
{
    u32 n = r16(a[1]), src = r32(a[1] + 4), dst;
    if (a[2]) {
        dst = galloc(n * 2 + 2);
        w32(a[0] + 4, dst);
        w16(a[0] + 2, (u16)(n * 2 + 2));
    } else {
        dst = r32(a[0] + 4);
        if (r16(a[0] + 2) < n * 2)
            return 0x80000005; /* STATUS_BUFFER_OVERFLOW */
    }
    for (u32 i = 0; i < n; i++)
        w16(dst + i * 2, r8(src + i));
    if (a[2] || r16(a[0] + 2) >= n * 2 + 2)
        w16(dst + n * 2, 0);
    w16(a[0], (u16)(n * 2));
    return 0;
}
API(RtlUnicodeStringToAnsiString) /* (ansi destination, unicode source, allocate) */
{
    u32 n = r16(a[1]) / 2, src = r32(a[1] + 4), dst;
    if (a[2]) {
        dst = galloc(n + 1);
        w32(a[0] + 4, dst);
        w16(a[0] + 2, (u16)(n + 1));
    } else {
        dst = r32(a[0] + 4);
        if (r16(a[0] + 2) < n)
            return 0x80000005;
    }
    for (u32 i = 0; i < n; i++) {
        u16 c = r16(src + i * 2);
        w8(dst + i, c < 256 ? (u8)c : '?');
    }
    if (a[2] || r16(a[0] + 2) >= n + 1)
        w8(dst + n, 0);
    w16(a[0], (u16)n);
    return 0;
}
API(RtlAnsiCharToUnicodeChar)
{
    u32 p = r32(a[0]), c = r8(p);
    w32(a[0], p + 1);
    return c;
}
API(RtlFreeUnicodeString) { gfree(r32(a[0] + 4)); w32(a[0] + 4, 0); w32(a[0], 0); return 0; }
API(RtlFreeAnsiString) { return k_RtlFreeUnicodeString(a); }
API(RtlNtStatusToDosError) { return a[0] ? 31 : 0; }

static struct module *module_at(u32 base)
{
    for (int i = 0; i < nmods; i++)
        if (!mods[i].builtin && mods[i].base == base)
            return &mods[i];
    return NULL;
}

API(RtlImageNtHeader)
{
    u32 b = a[0];
    if (!b || r16(b) != 0x5a4d || r32(b + r32(b + 0x3c)) != 0x4550)
        return 0;
    return b + r32(b + 0x3c);
}

/* IMAGEHLP: the checksum of a program file. It is the sum of the 16-bit words of the file
   (a carry goes back into the sum, the checksum field counts as 0) plus the length of the file. */
static u32 pe_checksum(const u8 *p, u32 len, u32 *old)
{
    u32 sum = 0, field = 0xffffffff, pe;

    *old = 0;
    if (len >= 0x40 && p[0] == 'M' && p[1] == 'Z') {
        memcpy(&pe, p + 0x3c, 4);
        if (pe < len && len - pe >= 24 + 68 && !memcmp(p + pe, "PE\0\0", 4)) {
            field = pe + 24 + 64;
            memcpy(old, p + field, 4);
        }
    }
    for (u32 i = 0; i < len; i += 2) {
        u32 w = p[i] | (i + 1 < len ? (u32)p[i + 1] << 8 : 0);
        if (i == field || i == field + 2)
            w = 0;
        sum += w;
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    return sum + len;
}

API(CheckSumMappedFile) /* (address, length, &the sum in the file, &the right sum): the PE header */
{
    u32 old, sum = pe_checksum(G + a[0], a[1], &old);

    if (a[2])
        w32(a[2], old);
    if (a[3])
        w32(a[3], sum);
    return k_RtlImageNtHeader(a);
}

API(MapFileAndCheckSumA) /* (file name, &the sum in the file, &the right sum): 0 is good */
{
    char hp[PATH_MAX];
    struct stat st;
    u8 *p;
    u32 old, sum;
    int fd;

    to_host(gstr(a[0]), hp, sizeof hp);
    fd = open(hp, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0)
            close(fd);
        return 1; /* CHECKSUM_OPEN_FAILURE */
    }
    p = malloc((size_t)st.st_size + 2);
    if (read(fd, p, (size_t)st.st_size) != st.st_size) {
        close(fd);
        free(p);
        return 3; /* CHECKSUM_MAPVIEW_FAILURE */
    }
    close(fd);
    sum = pe_checksum(p, (u32)st.st_size, &old);
    free(p);
    if (a[1])
        w32(a[1], old);
    if (a[2])
        w32(a[2], sum);
    return 0;
}
API(RtlImageDirectoryEntryToData) /* (base, mapped as image, directory, size) */
{
    u32 b = a[0], nt, opt, rva, n = a[2];
    if (!b || r16(b) != 0x5a4d)
        return 0;
    nt = b + r32(b + 0x3c);
    opt = nt + 24;
    if (n >= r32(opt + 92))
        return 0;
    rva = r32(opt + 96 + n * 8);
    if (a[3])
        w32(a[3], r32(opt + 100 + n * 8));
    if (!rva)
        return 0;
    if (!a[1] && !module_at(b)) { /* a file mapped as data: the offset in the file */
        u32 sec = opt + r16(nt + 20), ns = r16(nt + 6);
        for (u32 s = 0; s < ns; s++, sec += 40)
            if (rva >= r32(sec + 12) && rva < r32(sec + 12) + r32(sec + 16))
                return b + rva - r32(sec + 12) + r32(sec + 20);
        return 0;
    }
    return b + rva;
}

/* a resource directory entry with this id: its data offset (high bit: a directory), 0 if none */
/* id: a number, or a guest string when it is 0x10000 or more ("#12" is the number 12) */
static u32 res_find(u32 rbase, u32 dir, u32 id, int any)
{
    u32 named = r16(rbase + dir + 12), ids = r16(rbase + dir + 14), e = rbase + dir + 16;
    const char *str = NULL;

    if (id >= 0x10000) {
        str = gstr(id);
        if (str[0] == '#') {
            id = (u32)atoi(str + 1);
            str = NULL;
        }
    }
    for (u32 k = 0; k < named + ids; k++, e += 8) {
        u32 nm = r32(e);
        if (any)
            return r32(e + 4);
        if (str) {
            if (nm & 0x80000000u) {
                u32 s = rbase + (nm & 0x7fffffff), len = r16(s), j;
                for (j = 0; j < len && str[j]; j++)
                    if (mapchar(r16(s + 2 + 2 * j), 0x200) != mapchar((u8)str[j], 0x200))
                        break;
                if (j == len && !str[j])
                    return r32(e + 4);
            }
        } else if (nm == id) {
            return r32(e + 4);
        }
    }
    return 0;
}

/* the data entry of a resource (address of: data RVA, size, code page, 0), in any language; 0 if none */
static u32 res_entry(u32 hmod, u32 type, u32 name, u32 *modbase)
{
    struct module *m = &mods[0];
    u32 opt, rrva, rbase, d;

    for (int i = 0; i < nmods; i++)
        if (hmod && mods[i].base == hmod)
            m = &mods[i];
    if (m->builtin)
        return 0;
    opt = m->base + r32(m->base + 0x3c) + 24;
    rrva = r32(opt + 92) > 2 ? r32(opt + 96 + 16) : 0;
    if (!rrva)
        return 0;
    rbase = m->base + rrva;
    d = res_find(rbase, 0, type, 0);
    if (!(d & 0x80000000u))
        return 0;
    d = res_find(rbase, d & 0x7fffffff, name, 0);
    if (!(d & 0x80000000u))
        return 0;
    d = res_find(rbase, d & 0x7fffffff, 0, 1);
    if (!d || (d & 0x80000000u))
        return 0;
    if (modbase)
        *modbase = m->base;
    return rbase + d;
}

API(FindResourceA)
{
    u32 e = res_entry(a[0], a[2], a[1], NULL);
    if (!e)
        seterr(1814);
    return e;
}
API(LoadResource)
{
    u32 base = mods[0].base;
    for (int i = 0; i < nmods; i++)
        if (a[0] && mods[i].base == a[0])
            base = mods[i].base;
    return a[1] ? base + r32(a[1]) : 0;
}
API(SizeofResource) { return a[1] ? r32(a[1] + 4) : 0; }
API(LockResource) { return a[0]; }
API(FreeResource) { UNUSED; return 0; }

API(LoadStringA)
{
    u32 base = 0, e = res_entry(a[0], 6, (a[1] >> 4) + 1, &base), p, len;

    if (a[3])
        w8(a[2], 0);
    if (!e)
        return 0;
    p = base + r32(e);
    for (u32 k = 0; k < (a[1] & 15); k++)
        p += 2 + 2 * r16(p);
    len = r16(p);
    if (!a[3])
        return len;
    if (len > a[3] - 1)
        len = a[3] - 1;
    for (u32 k = 0; k < len; k++) {
        u16 c = r16(p + 2 + 2 * k);
        w8(a[2] + k, c < 256 ? (u8)c : '?');
    }
    w8(a[2] + len, 0);
    return len;
}

/* the text of a message: from a string, from the message table of a module, or of the system */
static const char *sys_message(u32 id)
{
    static const struct { u32 id; const char *text; } t[] = {
        { 0, "The operation completed successfully." }, { 1, "Incorrect function." },
        { 2, "The system cannot find the file specified." }, { 3, "The system cannot find the path specified." },
        { 4, "The system cannot open the file." }, { 5, "Access is denied." }, { 6, "The handle is invalid." },
        { 8, "Not enough storage is available to process this command." },
        { 15, "The system cannot find the drive specified." }, { 18, "There are no more files." },
        { 32, "The process cannot access the file because it is being used by another process." },
        { 80, "The file exists." }, { 87, "The parameter is incorrect." },
        { 112, "There is not enough space on the disk." },
        { 123, "The filename, directory name, or volume label syntax is incorrect." },
        { 145, "The directory is not empty." }, { 183, "Cannot create a file when that file already exists." },
        { 193, "%1 is not a valid Windows NT application." }, { 206, "The filename or extension is too long." },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].id == id)
            return t[i].text;
    return NULL;
}

/* FormatMessage: argument K (0 is the first) of an argument array or of a va_list */
static u32 fm_arg(u32 flags, u32 args, u32 k)
{
    if (flags & 0x2000) /* ARGUMENT_ARRAY */
        return r32(args + 4 * k);
    if (machine == M_ALPHA) /* the va_list of Alpha: a pointer and an offset, 8 bytes for each argument */
        return r32(r32(args) + r32(args + 4) + 8 * k);
    return r32(r32(args) + 4 * k);
}

#define FM_INSERT_MAX 2048

static int fm_w; /* FormatMessageW is at work: "s" and "c" are UTF-16, "hs", "hc", "S" and "C" are ANSI */

/* FormatMessageA: the text of one insert. FMT is the printf format between the two "!" (without
   the "%"), V the argument, STAR the values for "*". "s" and "c" are ANSI; "ws", "ls", "S", "wc",
   "lc" and "C" are UTF-16. The text goes to OUT (FM_INSERT_MAX bytes); the result is its length. */
static u32 fm_insert(char *out, const char *fmt, u32 v, const u32 *star)
{
    char fl[8], spec[64], text[FM_INSERT_MAX];
    int nf = 0, width = -1, prec = -1, wide = fm_w, ns = 0, len;
    const char *p = fmt;

    while (*p && strchr("-+ #0", *p)) {
        if (nf < 6)
            fl[nf++] = *p;
        p++;
    }
    if (*p == '*') {
        width = (int)star[ns++];
        p++;
        if (width < 0) { /* a negative width: to the left */
            width = -width;
            if (nf < 7)
                fl[nf++] = '-';
        }
    } else if (*p >= '0' && *p <= '9') {
        for (width = 0; *p >= '0' && *p <= '9' && width < 10000; p++)
            width = width * 10 + (*p - '0');
    }
    fl[nf] = 0;
    if (*p == '.') {
        p++;
        prec = 0;
        if (*p == '*') {
            prec = (int)star[ns++];
            p++;
        } else {
            for (; *p >= '0' && *p <= '9' && prec < 10000; p++)
                prec = prec * 10 + (*p - '0');
        }
    }
    for (; *p == 'h' || *p == 'l' || *p == 'w'; p++)
        wide = *p != 'h';
    if (width > FM_INSERT_MAX - 1)
        width = FM_INSERT_MAX - 1;
    len = snprintf(spec, sizeof spec, "%%%s", fl);
    if (width >= 0)
        len += snprintf(spec + len, sizeof spec - (size_t)len, "%d", width);
    if (prec >= 0)
        len += snprintf(spec + len, sizeof spec - (size_t)len, ".%d", prec);
    switch (*p) {
    case 'S':
        wide = !fm_w;
        /* fall through */
    case 's':
        strcpy(spec + len, "s");
        if (!v) {
            strcpy(text, "(null)");
        } else if (wide) {
            u32 k;
            for (k = 0; k < sizeof text - 1 && r16(v + 2 * k); k++)
                text[k] = r16(v + 2 * k) < 256 ? (char)r16(v + 2 * k) : '?';
            text[k] = 0;
        } else {
            snprintf(text, sizeof text, "%s", gstr(v));
        }
        len = snprintf(out, FM_INSERT_MAX, spec, text);
        break;
    case 'C':
        wide = !fm_w;
        /* fall through */
    case 'c':
        strcpy(spec + len, "c");
        len = snprintf(out, FM_INSERT_MAX, spec, wide && (v & 0xffff) > 255 ? '?' : (int)(v & 0xff));
        break;
    case 'd':
    case 'i':
        strcpy(spec + len, "d");
        len = snprintf(out, FM_INSERT_MAX, spec, (int)v);
        break;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
        spec[len] = *p;
        spec[len + 1] = 0;
        len = snprintf(out, FM_INSERT_MAX, spec, v);
        break;
    case 'p':
        len = snprintf(out, FM_INSERT_MAX, "%08X", v);
        break;
    default: /* not a format ntrun knows: the format text itself */
        len = snprintf(out, FM_INSERT_MAX, "%%%s", fmt);
    }
    if (len < 0)
        len = 0;
    if (len > FM_INSERT_MAX - 1)
        len = FM_INSERT_MAX - 1;
    return (u32)len;
}

API(FormatMessageA) /* (flags, source, message id, language, buffer, size, arguments) */
{
    static char src[32768], out[65536];
    u32 flags = a[0], args = a[6], n = 0, used = 0;
    const char *s;

    src[0] = 0;
    if (flags & 0x400) { /* FROM_STRING */
        snprintf(src, sizeof src, "%s", gstr(a[1]));
    } else {
        int found = 0;
        if (flags & 0x800) { /* FROM_HMODULE: the message table is resource type 11, name 1 */
            u32 base = 0, e = res_entry(a[1], 11, 1, &base);
            if (e) {
                u32 t = base + r32(e), nb = r32(t);
                for (u32 b = 0; b < nb && !found; b++) {
                    u32 lo = r32(t + 4 + 12 * b), hi = r32(t + 8 + 12 * b), p = t + r32(t + 12 + 12 * b);
                    if (a[2] < lo || a[2] > hi)
                        continue;
                    for (u32 id = lo; id < a[2]; id++)
                        p += r16(p);
                    if (r16(p + 2) & 1) { /* Unicode text */
                        u32 len = (r16(p) - 4u) / 2, j;
                        for (j = 0; j < len && j < sizeof src - 1 && r16(p + 4 + 2 * j); j++)
                            src[j] = (char)r16(p + 4 + 2 * j);
                        src[j] = 0;
                    } else {
                        snprintf(src, sizeof src, "%.*s", (int)(r16(p) - 4), gstr(p + 4));
                    }
                    found = 1;
                }
            }
        }
        if (!found && (flags & 0x1000)) { /* FROM_SYSTEM */
            const char *m = sys_message(a[2]);
            if (m)
                snprintf(src, sizeof src, "%s\r\n", m);
            else
                snprintf(src, sizeof src, "System error %u.\r\n", a[2]);
            found = 1;
        }
        if (!found) {
            seterr(317);
            return 0;
        }
    }
    /* the low byte of the flags is the line width: 0 keeps the line ends of the text,
       another value makes them spaces (ntrun does not wrap the lines) */
    for (s = src; *s && n < sizeof out - FM_INSERT_MAX - 8;) {
        if (*s == '\r' || *s == '\n') {
            if (*s == '\r' && s[1] == '\n')
                s++;
            s++;
            if (flags & 0xff) {
                out[n++] = ' ';
            } else {
                out[n++] = '\r';
                out[n++] = '\n';
            }
            continue;
        }
        if (*s != '%') {
            out[n++] = *s++;
            continue;
        }
        s++;
        if (*s >= '1' && *s <= '9') { /* an insert: %1 to %99, then an optional !format! */
            u32 num = (u32)(*s++ - '0'), nstar = 0, star[2] = { 0, 0 }, v;
            char fmt[32] = "s";
            int hasfmt = 0;
            if (*s >= '0' && *s <= '9')
                num = num * 10 + (u32)(*s++ - '0');
            if (*s == '!') {
                const char *e = strchr(s + 1, '!');
                if (!e || e - s - 1 >= (long)sizeof fmt) {
                    seterr(87);
                    return 0;
                }
                memcpy(fmt, s + 1, (size_t)(e - s - 1));
                fmt[e - s - 1] = 0;
                s = e + 1;
                hasfmt = 1;
            }
            if ((flags & 0x200) || !args) { /* IGNORE_INSERTS, or no arguments: the insert stays as it is */
                n += (u32)snprintf(out + n, 64, hasfmt ? "%%%u!%s!" : "%%%u", num, fmt);
                continue;
            }
            for (const char *q = fmt; *q; q++)
                if (*q == '*' && nstar < 2)
                    nstar++;
            if (num > used)
                used = num;
            v = fm_arg(flags, args, num - 1);
            for (u32 k = 0; k < nstar; k++) /* a "*" takes the argument after the last one used */
                star[k] = fm_arg(flags, args, used++);
            n += fm_insert(out + n, fmt, v, star);
            continue;
        }
        if (*s == '0' || !*s) /* %0: the message ends here, with no new line */
            break;
        switch (*s) {
        case 'n': out[n++] = '\r'; out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'b': out[n++] = ' '; break;
        case '!': out[n++] = '!'; break;
        default: /* "%%", "%." and so on: the character */
            if (flags & 0x200)
                out[n++] = '%';
            out[n++] = *s;
        }
        s++;
    }
    out[n] = 0;
    if (flags & 0x100) { /* ALLOCATE_BUFFER */
        w32(a[4], gdup(out, n + 1));
    } else {
        if (n + 1 > a[5]) {
            seterr(122);
            return 0;
        }
        memcpy(G + a[4], out, n + 1);
    }
    return n;
}

/* wsprintfA(buf, fmt, ...): the arguments are 32-bit words after the two pointers */
API(wsprintfA)
{
    const char *f = gstr(a[1]);
    char *out = gstr(a[0]), spec[32], tmp[1100];
    u32 n = 0;
    int ai = 2;

    while (*f && n < 1024) {
        size_t l;
        if (*f != '%') {
            out[n++] = *f++;
            continue;
        }
        l = 1;
        while (f[l] && strchr("-#0123456789.l", f[l]))
            l++;
        if (!f[l] || l + 2 > sizeof spec)
            break;
        memcpy(spec, f, l + 1);
        spec[l + 1] = 0;
        if (f[l] == '%') {
            out[n++] = '%';
        } else {
            u32 v = ai < 16 ? a[ai++] : 0;
            char *lp;
            while ((lp = strchr(spec, 'l')))
                memmove(lp, lp + 1, strlen(lp));
            if (f[l] == 's')
                snprintf(tmp, sizeof tmp, spec, v ? gstr(v) : "(null)");
            else if (f[l] == 'c')
                snprintf(tmp, sizeof tmp, spec, (int)v);
            else if (strchr("dixXu", f[l]))
                snprintf(tmp, sizeof tmp, spec, v);
            else
                snprintf(tmp, sizeof tmp, "%s", spec);
            {
                size_t tl = strlen(tmp);
                if (n + tl > 1024)
                    tl = 1024 - n;
                memcpy(out + n, tmp, tl);
                n += (u32)tl;
            }
        }
        f += l + 1;
    }
    out[n] = 0;
    return n;
}

/* wide character functions: through the one-byte ones */
static u32 w2a(u32 w)
{
    u32 n = 0, p;
    if (!w)
        return 0;
    while (r16(w + n * 2))
        n++;
    p = galloc(n + 1);
    for (u32 i = 0; i <= n; i++) {
        u16 c = r16(w + i * 2);
        w8(p + i, c < 256 ? (u8)c : (u8)0x3f);
    }
    return p;
}

static u32 a2w(const char *s, u32 n)
{
    u32 p = galloc(n * 2 + 2);
    for (u32 i = 0; i < n; i++)
        w16(p + i * 2, (u8)s[i]);
    w16(p + n * 2, 0);
    return p;
}

API(GetCommandLineW)
{
    static u32 w;
    UNUSED;
    if (!w)
        w = a2w(gstr(cmdline_g), (u32)strlen(gstr(cmdline_g)));
    return w;
}

#define W1(name) API(name##W) { u32 s = w2a(a[0]), b[8] = { s, a[1], a[2], a[3], a[4], a[5], a[6], a[7] }, r = k_##name##A(b); gfree(s); return r; }
W1(CreateFile) W1(GetFileAttributes) W1(SetFileAttributes) W1(DeleteFile) W1(CreateDirectory) W1(RemoveDirectory)
W1(SetCurrentDirectory) W1(GetDriveType)

API(MoveFileW) { u32 s = w2a(a[0]), t = w2a(a[1]), b[2] = { s, t }, r = k_MoveFileA(b); gfree(s); gfree(t); return r; }
API(SetEnvironmentVariableW) { u32 s = w2a(a[0]), t = w2a(a[1]), b[2] = { s, t }, r = k_SetEnvironmentVariableA(b); gfree(s); gfree(t); return r; }

API(GetModuleFileNameW)
{
    u32 tmp = galloc(PATH_MAX), b[3] = { a[0], tmp, PATH_MAX }, n = k_GetModuleFileNameA(b);
    if (n + 1 > a[2])
        n = a[2] ? a[2] - 1 : 0;
    for (u32 i = 0; i < n; i++)
        w16(a[1] + i * 2, r8(tmp + i));
    w16(a[1] + n * 2, 0);
    gfree(tmp);
    return n;
}

API(GetCurrentDirectoryW)
{
    u32 tmp = galloc(PATH_MAX), b[2] = { PATH_MAX, tmp }, n = k_GetCurrentDirectoryA(b);
    if (n + 1 > a[0]) {
        gfree(tmp);
        return n + 1;
    }
    for (u32 i = 0; i <= n; i++)
        w16(a[1] + i * 2, r8(tmp + i));
    gfree(tmp);
    return n;
}

API(GetFullPathNameW)
{
    u32 s = w2a(a[0]), tmp = galloc(PATH_MAX), part = galloc(4), b[4] = { s, PATH_MAX, tmp, part }, n = k_GetFullPathNameA(b);
    if (n + 1 <= a[1]) {
        for (u32 i = 0; i <= n; i++)
            w16(a[2] + i * 2, r8(tmp + i));
        if (a[3])
            w32(a[3], r32(part) ? a[2] + (r32(part) - tmp) * 2 : 0);
    } else {
        n++;
    }
    gfree(s);
    gfree(tmp);
    gfree(part);
    return n;
}

static void find_a2w(u32 fa, u32 fw)
{
    memcpy(G + fw, G + fa, 44);
    for (u32 i = 0; i < 260; i++)
        w16(fw + 44 + i * 2, r8(fa + 44 + i));
    for (u32 i = 0; i < 14; i++)
        w16(fw + 44 + 520 + i * 2, r8(fa + 44 + 260 + i));
}

API(FindFirstFileW)
{
    u32 s = w2a(a[0]), fa = galloc(320), b[2] = { s, fa }, h = k_FindFirstFileA(b);
    if (h != INVALID)
        find_a2w(fa, a[1]);
    gfree(s);
    gfree(fa);
    return h;
}

API(FindNextFileW)
{
    u32 fa = galloc(320), b[2] = { a[0], fa }, r = k_FindNextFileA(b);
    if (r)
        find_a2w(fa, a[1]);
    gfree(fa);
    return r;
}

API(CompareStringW)
{
    s32 n1 = (s32)a[3], n2 = (s32)a[5];
    int ic = a[1] & 1;
    if (n1 < 0) for (n1 = 0; r16(a[2] + (u32)n1 * 2); n1++) ;
    if (n2 < 0) for (n2 = 0; r16(a[4] + (u32)n2 * 2); n2++) ;
    for (s32 i = 0; i < n1 && i < n2; i++) {
        u32 c1 = r16(a[2] + (u32)i * 2), c2 = r16(a[4] + (u32)i * 2);
        if (ic) { c1 = mapchar(c1, 0x200); c2 = mapchar(c2, 0x200); }
        if (c1 != c2)
            return c1 < c2 ? 1 : 3;
    }
    return n1 == n2 ? 2 : n1 < n2 ? 1 : 3;
}

API(GetLocaleInfoW)
{
    u32 tmp = galloc(256), b[4] = { a[0], a[1], tmp, 256 }, n = k_GetLocaleInfoA(b);
    if (a[3]) {
        if (n > a[3]) {
            gfree(tmp);
            seterr(122);
            return 0;
        }
        for (u32 i = 0; i < n; i++)
            w16(a[2] + i * 2, r8(tmp + i));
    }
    gfree(tmp);
    return n;
}

/* ---------------- for the tools of 2005 (the IA-64 compiler of the Platform SDK) ----------------
   Most are the wide character variants of functions above. */

#define WK(name, k) API(name##W) { u32 b[8], s = w2a(a[k]), r; memcpy(b, a, sizeof b); b[k] = s; r = k_##name##A(b); gfree(s); return r; }
WK(CreateEvent, 3) WK(CreateMutex, 2) WK(CreateSemaphore, 3) WK(CreateFileMapping, 5) WK(LoadLibrary, 0)
WK(GetModuleHandle, 0)

/* the text S of N characters to a buffer of SIZE wide characters: the length, or the size it needs */
static u32 out_w(u32 buf, u32 size, u32 s, u32 n)
{
    if (n + 1 > size)
        return n + 1;
    for (u32 i = 0; i < n; i++)
        w16(buf + i * 2, r8(s + i));
    w16(buf + n * 2, 0);
    return n;
}

API(InitializeCriticalSectionAndSpinCount) { k_InitializeCriticalSection(a); return 1; }
API(SetProcessWorkingSetSize) { UNUSED; return 1; }
API(GetUserDefaultUILanguage) { UNUSED; return 0x409; }
API(IsDebuggerPresent) { UNUSED; return 0; }
API(LoadLibraryExA) { return k_LoadLibraryA(a); } /* also a file with resources only: it has no start function */
API(LoadLibraryExW) { return k_LoadLibraryW(a); }
API(GetStartupInfoW) { return k_GetStartupInfoA(a); } /* the three text pointers are NULL */
API(ReadConsoleInputW) { return k_ReadConsoleInputA(a); }
API(CopyFileW) { u32 s = w2a(a[0]), t = w2a(a[1]), b[3] = { s, t, a[2] }, r = k_CopyFileA(b); gfree(s); gfree(t); return r; }

API(ReadConsoleW)
{
    u32 tmp = galloc(a[2] + 1), b[5] = { a[0], tmp, a[2], a[3], 0 }, r = k_ReadFile(b);
    if (r)
        for (u32 i = 0; i < r32(a[3]); i++)
            w16(a[1] + i * 2, r8(tmp + i));
    gfree(tmp);
    return r;
}

API(GetVersionExW)
{
    static const char sp[] = "Service Pack 3";
    u32 p = a[0];
    if (r32(p) < 276) {
        seterr(122);
        return 0;
    }
    w32(p + 4, 4);    /* Windows NT 4.0, build 1381 */
    w32(p + 8, 0);
    w32(p + 12, 1381);
    w32(p + 16, 2);
    for (u32 i = 0; i < sizeof sp; i++)
        w16(p + 20 + i * 2, (u8)sp[i]);
    if (r32(p) >= 284) { /* the longer form: service pack 3.0, a workstation */
        w16(p + 276, 3);
        w16(p + 278, 0);
        w16(p + 280, 0);
        w8(p + 282, 1);
        w8(p + 283, 0);
    }
    return 1;
}

API(GetEnvironmentVariableW)
{
    u32 s = w2a(a[0]), tmp = galloc(32768), b[3] = { s, tmp, 32768 }, n = k_GetEnvironmentVariableA(b);
    if (n)
        n = out_w(a[1], a[2], tmp, n);
    gfree(s);
    gfree(tmp);
    return n;
}

API(ExpandEnvironmentStringsW) /* the result counts the 0 at the end */
{
    u32 s = w2a(a[0]), tmp = galloc(32768), b[3] = { s, tmp, 32768 }, n = k_ExpandEnvironmentStringsA(b);
    if (n && n <= a[2])
        for (u32 i = 0; i < n; i++)
            w16(a[1] + i * 2, r8(tmp + i));
    gfree(s);
    gfree(tmp);
    return n;
}

API(GetTempPathW)
{
    u32 tmp = galloc(PATH_MAX), b[2] = { PATH_MAX, tmp }, n = k_GetTempPathA(b);
    n = out_w(a[1], a[0], tmp, n);
    gfree(tmp);
    return n;
}

API(GetTempFileNameW)
{
    u32 s = w2a(a[0]), t = w2a(a[1]), tmp = galloc(PATH_MAX), b[4] = { s, t, a[2], tmp }, r = k_GetTempFileNameA(b);
    if (r)
        out_w(a[3], 260, tmp, (u32)strlen(gstr(tmp)));
    gfree(s);
    gfree(t);
    gfree(tmp);
    return r;
}

API(GetShortPathNameW)
{
    u32 s = w2a(a[0]), tmp = galloc(PATH_MAX), b[3] = { s, tmp, PATH_MAX }, n = k_GetShortPathNameA(b);
    if (n)
        n = out_w(a[1], a[2], tmp, n);
    gfree(s);
    gfree(tmp);
    return n;
}

API(GetLongPathNameW) /* ntrun has no short names: the name itself, when the file is there */
{
    u32 s = w2a(a[0]), b[1] = { s }, n = (u32)strlen(gstr(s));
    if (k_GetFileAttributesA(b) == INVALID)
        n = 0;
    else
        n = out_w(a[1], a[2], s, n);
    gfree(s);
    return n;
}

API(GetComputerNameW)
{
    u32 tmp = galloc(64), sz = galloc(4), b[2] = { tmp, sz }, r, n;
    w32(sz, 64);
    r = k_GetComputerNameA(b);
    n = r32(sz);
    if (r && n + 1 > r32(a[1])) {
        seterr(111);
        r = 0;
    } else if (r) {
        out_w(a[0], n + 1, tmp, n);
    }
    w32(a[1], r ? n : n + 1);
    gfree(tmp);
    gfree(sz);
    return r;
}

API(GetDiskFreeSpaceExW) /* the same numbers as GetDiskFreeSpaceA: 4 GB of 8 GB are free */
{
    if (a[1]) w64(a[1], 0x100000000ull);
    if (a[2]) w64(a[2], 0x200000000ull);
    if (a[3]) w64(a[3], 0x100000000ull);
    return 1;
}

API(GetFileAttributesExA) /* (name, 0, attributes + three times + size) */
{
    char hp[PATH_MAX];
    struct stat st;
    u32 p = a[2];
    to_host(gstr(a[0]), hp, sizeof hp);
    if (a[1] || stat(hp, &st) < 0) {
        seterr(a[1] ? 87 : werr(errno));
        return 0;
    }
    w32(p, attrs_of(&st));
    w64(p + 4, unix_to_ft(st.st_mtime, 0));
    w64(p + 12, unix_to_ft(st.st_atime, 0));
    w64(p + 20, unix_to_ft(st.st_mtime, 0));
    w32(p + 28, (u32)((u64)st.st_size >> 32));
    w32(p + 32, (u32)st.st_size);
    return 1;
}
WK(GetFileAttributesEx, 0)

API(FindResourceW) /* (module, name, type): a name is a number or a text */
{
    u32 n = a[1] >= 0x10000 ? w2a(a[1]) : a[1], t = a[2] >= 0x10000 ? w2a(a[2]) : a[2];
    u32 b[3] = { a[0], n, t }, r = k_FindResourceA(b);
    if (a[1] >= 0x10000) gfree(n);
    if (a[2] >= 0x10000) gfree(t);
    return r;
}

API(LoadStringW) /* with size 0: the address of the text in the resources goes to the buffer */
{
    u32 base = 0, e = res_entry(a[0], 6, (a[1] >> 4) + 1, &base), p, len;

    if (a[3])
        w16(a[2], 0);
    if (!e)
        return 0;
    p = base + r32(e);
    for (u32 k = 0; k < (a[1] & 15); k++)
        p += 2 + 2 * r16(p);
    len = r16(p);
    if (!a[3]) {
        w32(a[2], p + 2);
        return len;
    }
    if (len > a[3] - 1)
        len = a[3] - 1;
    memmove(G + a[2], G + p + 2, len * 2);
    w16(a[2] + len * 2, 0);
    return len;
}

API(FormatMessageW) /* through FormatMessageA: the text and the "s" inserts are UTF-16 here */
{
    u32 b[8], src = 0, tmp = galloc(65536), n;

    memcpy(b, a, sizeof b);
    if (a[0] & 0x400)
        b[1] = src = w2a(a[1]);
    b[0] &= ~0x100u;
    b[4] = tmp;
    b[5] = 65536;
    fm_w = 1;
    n = k_FormatMessageA(b);
    fm_w = 0;
    if (!n) {
        /* the error is set */
    } else if (a[0] & 0x100) {
        w32(a[4], a2w(gstr(tmp), n));
    } else if (n + 1 > a[5]) {
        seterr(122);
        n = 0;
    } else {
        out_w(a[4], a[5], tmp, n);
    }
    gfree(src);
    gfree(tmp);
    return n;
}

static int wcmp(u32 x, u32 y, int nocase)
{
    for (;; x += 2, y += 2) {
        u32 c1 = r16(x), c2 = r16(y);
        if (nocase) {
            c1 = mapchar(c1, 0x200);
            c2 = mapchar(c2, 0x200);
        }
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
        if (!c1)
            return 0;
    }
}
API(lstrcmpW) { return (u32)wcmp(a[0], a[1], 0); }
API(lstrcmpiW) { return (u32)wcmp(a[0], a[1], 1); }
API(lstrcpyW)
{
    u32 n = 0;
    if (!a[0] || !a[1])
        return 0;
    while (r16(a[1] + n * 2))
        n++;
    memmove(G + a[0], G + a[1], n * 2 + 2);
    return a[0];
}
API(lstrcpynW) /* (to, from, size): at most size - 1 characters, and a 0 */
{
    u32 n = 0;
    if (!a[0] || !a[1] || !a[2])
        return 0;
    while (n + 1 < a[2] && r16(a[1] + n * 2))
        n++;
    memmove(G + a[0], G + a[1], n * 2);
    w16(a[0] + n * 2, 0);
    return a[0];
}
API(lstrcatW)
{
    u32 n = 0, b[2];
    if (!a[0] || !a[1])
        return 0;
    while (r16(a[0] + n * 2))
        n++;
    b[0] = a[0] + n * 2;
    b[1] = a[1];
    k_lstrcpyW(b);
    return a[0];
}
API(CharNextW) { return r16(a[0]) ? a[0] + 2 : a[0]; }
API(CharUpperW) /* a text, or one character in the low word */
{
    if (a[0] < 0x10000)
        return mapchar(a[0], 0x200);
    for (u32 p = a[0]; r16(p); p += 2)
        w16(p, (u16)mapchar(r16(p), 0x200));
    return a[0];
}

/* GetDateFormatA and GetTimeFormatA (locale, flags, time, picture, buffer, size): the pictures
   of Windows. d, M, y and g for a date; h, H, m, s and t for a time; text in single quotes. */
static u32 date_time_format(u32 *a, int date)
{
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
        "September", "October", "November", "December" };
    static const int off[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    const char *pic = a[3] ? gstr(a[3]) : date ? ((a[1] & 2) ? "dddd, MMMM dd, yyyy" : "M/d/yyyy") : "h:mm:ss tt";
    char out[512];
    u32 n = 0, st[8];

    if (a[2]) {
        for (int i = 0; i < 8; i++)
            st[i] = r16(a[2] + 2 * (u32)i);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        st[0] = (u32)tm.tm_year + 1900;
        st[1] = (u32)tm.tm_mon + 1;
        st[3] = (u32)tm.tm_mday;
        st[4] = (u32)tm.tm_hour;
        st[5] = (u32)tm.tm_min;
        st[6] = (u32)tm.tm_sec;
    }
    if (st[1] < 1 || st[1] > 12) {
        seterr(87);
        return 0;
    }
    {
        u32 y = st[0] - (st[1] < 3);
        st[2] = (y + y / 4 - y / 100 + y / 400 + (u32)off[st[1] - 1] + st[3]) % 7; /* the day of the week */
    }
    while (*pic && n < sizeof out - 40) {
        char c = *pic;
        int k = 0;
        if (c == '\'') { /* text; two quotes are one quote */
            for (pic++; *pic && n < sizeof out - 40; pic++) {
                if (*pic == '\'') {
                    if (pic[1] != '\'')
                        break;
                    pic++;
                }
                out[n++] = *pic;
            }
            if (*pic)
                pic++;
            continue;
        }
        while (pic[k] == c)
            k++;
        pic += k;
        if (date && c == 'd' && k >= 3)
            n += (u32)sprintf(out + n, k == 3 ? "%.3s" : "%s", days[st[2]]);
        else if (date && c == 'd')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[3]);
        else if (date && c == 'M' && k >= 3)
            n += (u32)sprintf(out + n, k == 3 ? "%.3s" : "%s", months[st[1] - 1]);
        else if (date && c == 'M')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[1]);
        else if (date && c == 'y' && k <= 2)
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[0] % 100);
        else if (date && c == 'y')
            n += (u32)sprintf(out + n, "%04u", st[0]);
        else if (date && c == 'g')
            n += (u32)sprintf(out + n, "A.D.");
        else if (!date && c == 'h')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[4] % 12 ? st[4] % 12 : 12);
        else if (!date && c == 'H')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[4]);
        else if (!date && c == 'm')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[5]);
        else if (!date && c == 's')
            n += (u32)sprintf(out + n, k == 1 ? "%u" : "%02u", st[6]);
        else if (!date && c == 't')
            n += (u32)sprintf(out + n, k == 1 ? "%c" : "%cM", st[4] < 12 ? 'A' : 'P');
        else
            while (k-- && n < sizeof out - 40)
                out[n++] = c;
    }
    out[n] = 0;
    if (!a[5])
        return n + 1;
    if (n + 1 > a[5]) {
        seterr(122);
        return 0;
    }
    memcpy(G + a[4], out, n + 1);
    return n + 1;
}
API(GetDateFormatA) { return date_time_format(a, 1); }
API(GetTimeFormatA) { return date_time_format(a, 0); }

/* ADVAPI32: the registry has nothing; there is no token of a user */
API(RegOpenKeyExW) { UNUSED; return 2; }
API(RegQueryValueExW) { UNUSED; return 2; }
API(OpenProcessToken) { UNUSED; seterr(120); return 0; }
API(GetTokenInformation) { UNUSED; seterr(120); return 0; }

/* ADVAPI32: MD5 and SHA-1 sums and random numbers. The compiler makes a sum of each source file. */
struct hash {
    int used, alg, done;
    u32 s[5], nbuf;
    u64 len;
    u8 buf[64], out[20];
};
static struct hash *hashes; /* the compiler keeps one for each file that it reads */
static u32 nhashes;

static u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void md5_block(u32 *s, const u8 *p)
{
    static const u8 r[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    static u32 k[64];
    u32 w[16], a = s[0], b = s[1], c = s[2], d = s[3];

    if (!k[0])
        for (int i = 0; i < 64; i++)
            k[i] = (u32)(u64)(fabs(sin(i + 1.0)) * 4294967296.0);
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        u32 f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        f += a + k[i] + w[g];
        a = d;
        d = c;
        c = b;
        b += rol32(f, r[i]);
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

static void sha1_block(u32 *s, const u8 *p)
{
    u32 w[80], a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];

    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | (u32)p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        u32 f, k, t;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
    s[4] += e;
}

static void hash_add(struct hash *h, const u8 *p, u32 n)
{
    h->len += n;
    while (n) {
        u32 k = 64 - h->nbuf;
        if (k > n)
            k = n;
        memcpy(h->buf + h->nbuf, p, k);
        h->nbuf += k;
        p += k;
        n -= k;
        if (h->nbuf == 64) {
            if (h->alg == 0x8003) md5_block(h->s, h->buf); else sha1_block(h->s, h->buf);
            h->nbuf = 0;
        }
    }
}

static void hash_end(struct hash *h)
{
    u64 bits = h->len * 8;
    u8 pad[72] = { 0x80 }, lenb[8];
    int md5 = h->alg == 0x8003;

    if (h->done)
        return;
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (md5 ? 8 * i : 56 - 8 * i));
    hash_add(h, pad, h->nbuf < 56 ? 56 - h->nbuf : 120 - h->nbuf);
    hash_add(h, lenb, 8);
    for (int i = 0; i < (md5 ? 16 : 20); i++)
        h->out[i] = (u8)(h->s[i / 4] >> (md5 ? 8 * (i % 4) : 24 - 8 * (i % 4)));
    h->done = 1;
}

API(CryptAcquireContextA) { w32(a[0], 0xc0de0001); return 1; }
API(CryptAcquireContextW) { w32(a[0], 0xc0de0001); return 1; }
API(CryptReleaseContext) { UNUSED; return 1; }
API(CryptGenRandom) /* (provider, length, buffer) */
{
#ifdef _WIN32
    for (u32 i = 0; i < a[1]; i++) {
        unsigned int v;
        if (rand_s(&v) != 0)
            v = (unsigned)rand();
        w8(a[2] + i, (u8)(v >> 8));
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, G + a[2], a[1]) != (ssize_t)a[1])
        for (u32 i = 0; i < a[1]; i++)
            w8(a[2] + i, (u8)(random() >> 8));
    if (fd >= 0)
        close(fd);
#endif
    return 1;
}
API(CryptCreateHash) /* (provider, algorithm, key, flags, the new handle) */
{
    static const u32 init[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    if (a[1] != 0x8003 && a[1] != 0x8004) { /* MD5, SHA-1 */
        seterr(0x80090008);
        return 0;
    }
    for (u32 i = 0;; i++) {
        if (i == nhashes) {
            nhashes = nhashes ? nhashes * 2 : 64;
            hashes = realloc(hashes, nhashes * sizeof hashes[0]);
            if (!hashes)
                die("no memory");
            memset(hashes + i, 0, (nhashes - i) * sizeof hashes[0]);
        }
        if (!hashes[i].used) {
            memset(&hashes[i], 0, sizeof hashes[i]);
            hashes[i].used = 1;
            hashes[i].alg = (int)a[1];
            memcpy(hashes[i].s, init, sizeof init);
            w32(a[4], 0xc0de1000 + i);
            return 1;
        }
    }
}
static struct hash *hash_of(u32 h)
{
    u32 i = h - 0xc0de1000;
    if (i >= nhashes || !hashes[i].used) {
        seterr(0x80090001);
        return NULL;
    }
    return &hashes[i];
}
API(CryptHashData) /* (hash, data, length, flags) */
{
    struct hash *h = hash_of(a[0]);
    if (!h || h->done) {
        if (h)
            seterr(0x8009000c);
        return 0;
    }
    hash_add(h, G + a[1], a[2]);
    return 1;
}
API(CryptGetHashParam) /* (hash, what, buffer, its size, flags): 1 the algorithm, 2 the sum, 4 its size */
{
    struct hash *h = hash_of(a[0]);
    u32 size, v;
    if (!h)
        return 0;
    size = a[1] == 2 ? (h->alg == 0x8003 ? 16 : 20) : 4;
    v = a[1] == 1 ? (u32)h->alg : h->alg == 0x8003 ? 16 : 20;
    if (a[1] != 1 && a[1] != 2 && a[1] != 4) {
        seterr(0x8009000a);
        return 0;
    }
    if (a[2] && r32(a[3]) < size) {
        w32(a[3], size);
        seterr(234);
        return 0;
    }
    if (a[2]) {
        if (a[1] == 2) {
            hash_end(h);
            memcpy(G + a[2], h->out, size);
        } else {
            w32(a[2], v);
        }
    }
    w32(a[3], size);
    return 1;
}
API(CryptDestroyHash)
{
    struct hash *h = hash_of(a[0]);
    if (!h)
        return 0;
    h->used = 0;
    return 1;
}

#define E(n) { #n, k_##n, 0 }
static const struct api builtin[] = {
    E(GetVersion), E(GetLastError), E(SetLastError), E(GetCurrentProcess), E(GetCurrentThread), E(GetCurrentProcessId),
    E(GetCurrentThreadId), E(GetTickCount), E(Sleep), E(Beep), E(SetErrorMode), E(SetHandleCount),
    E(SetConsoleCtrlHandler), E(SetUnhandledExceptionFilter), E(DisableThreadLibraryCalls), E(GetACP), E(GetOEMCP),
    E(IsValidCodePage), E(IsValidLocale), E(GetUserDefaultLCID), E(IsBadReadPtr), E(IsBadWritePtr), E(IsBadCodePtr),
    E(ExitProcess), E(TerminateProcess), E(DebugBreak), E(UnhandledExceptionFilter), E(RaiseException), E(GetCPInfo),
    E(GetSystemInfo), E(GetCommandLineA), E(GetEnvironmentStrings), E(GetEnvironmentStringsW),
    E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW), E(SetEnvironmentVariableA), E(GetStartupInfoA),
    E(GetStdHandle), E(SetStdHandle), E(GetFileType), E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapFree),
    E(HeapReAlloc), E(HeapSize), E(HeapValidate), E(HeapCompact), E(HeapLock), E(HeapUnlock), E(HeapWalk),
    E(VirtualAlloc), E(VirtualFree), E(VirtualProtect), E(CreateFileA), E(CloseHandle), E(ReadFile), E(WriteFile),
    E(SetFilePointer), E(SetEndOfFile), E(FlushFileBuffers), E(LockFile), E(UnlockFile), E(GetFileSize),
    E(GetFileAttributesA), E(SetFileAttributesA), E(GetFileTime), E(SetFileTime), E(GetFileInformationByHandle),
    E(DeleteFileA), E(MoveFileA), E(CreateDirectoryA), E(RemoveDirectoryA), E(GetCurrentDirectoryA),
    E(SetCurrentDirectoryA), E(GetFullPathNameA), E(GetDriveTypeA), E(GetLogicalDrives), E(GetDiskFreeSpaceA),
    E(FindFirstFileA), E(FindNextFileA), E(FindClose), E(GetConsoleMode), E(SetConsoleMode),
    E(GetConsoleScreenBufferInfo), E(WriteConsoleA), E(ReadConsoleA), E(MultiByteToWideChar),
    E(WideCharToMultiByte), E(GetStringTypeA), E(GetStringTypeW), E(LCMapStringA), E(LCMapStringW),
    E(CompareStringA), E(GetLocaleInfoA), E(FormatMessageA), E(GetSystemTime), E(GetLocalTime), E(SetLocalTime),
    E(GetTimeZoneInformation), E(FileTimeToSystemTime), E(SystemTimeToFileTime), E(FileTimeToLocalFileTime),
    E(LocalFileTimeToFileTime), E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
    E(InitializeCriticalSection), E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection),
    E(CreateSemaphoreA), E(ReleaseSemaphore), E(CreateMutexA), E(ReleaseMutex), E(RtlMoveMemory), E(RtlFillMemory),
    E(RtlZeroMemory), E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(GetModuleHandleA),
    E(GetModuleFileNameA), E(CreateFileMappingA), E(MapViewOfFileEx), E(MapViewOfFile), E(UnmapViewOfFile),
    E(CreateProcessA), E(WaitForSingleObject), E(WaitForMultipleObjects), E(TryEnterCriticalSection), E(GetExitCodeProcess), E(DuplicateHandle), E(CreatePipe),
    E(PeekNamedPipe), E(RtlUnwind), E(RtlLookupFunctionEntry), E(RtlVirtualUnwind), E(RtlCaptureContext),
    E(RtlRaiseException), E(RtlUnwindRfp), E(RtlUnwindReturn), E(setjmp), E(longjmp),
    E(LocalReAlloc), E(GlobalReAlloc), E(LocalSize), E(GlobalSize), E(LocalLock), E(LocalUnlock), E(IsCharLowerA),
    E(IsCharUpperA), E(IsCharAlphaA), E(IsCharAlphaNumericA), E(RtlCopyMemory), E(CreateProcessW),
    E(GetProcessHeap), E(LocalAlloc), E(LocalFree), E(GlobalAlloc), E(GlobalFree), E(CharToOemA), E(OemToCharA),
    E(CharNextA), E(CharUpperBuffW), E(SetFileApisToOEM), E(SetFileApisToANSI), E(AreFileApisANSI), E(GetConsoleCP),
    E(GetConsoleOutputCP), E(IsDBCSLeadByte), E(IsDBCSLeadByteEx), E(SendMessageA),
    E(GetNumberOfConsoleInputEvents), E(PeekConsoleInputA), E(ReadConsoleInputA), E(FatalAppExitA), E(MessageBoxA),
    E(WriteConsoleW), E(DosDateTimeToFileTime), E(FileTimeToDosDateTime), E(CopyFileA), E(GetTempPathA),
    E(GetTempFileNameA), E(wsprintfA), E(LoadStringA), E(FindResourceA), E(LoadResource), E(SizeofResource),
    E(LockResource), E(FreeResource),
    E(lstrlenA), E(lstrlenW), E(lstrcpyA), E(lstrcatA), E(lstrcmpA), E(lstrcmpiA), E(lstrcpynA), E(CharPrevA),
    E(CharUpperA), E(CharLowerA), E(CharUpperBuffA), E(CharLowerBuffA), E(IsBadStringPtrA), E(OutputDebugStringA),
    E(SendMessageW), E(GlobalLock), E(GlobalUnlock), E(GetSystemDefaultLCID), E(GetSystemDefaultLangID),
    E(GetUserDefaultLangID), E(GetThreadLocale), E(EnumSystemLocalesA), E(SetConsoleTitleA), E(SetThreadPriority),
    E(GetThreadPriority), E(SetPriorityClass), E(FlushViewOfFile), E(FlushInstructionCache),
    E(GetProcessAffinityMask), E(GetVersionExA), E(GetSystemTimeAsFileTime), E(QueryPerformanceFrequency),
    E(QueryPerformanceCounter), E(GlobalMemoryStatus), E(GetEnvironmentVariableA), E(ExpandEnvironmentStringsA),
    E(GetComputerNameA), E(GetUserNameA), E(GetShortPathNameA), E(GetSystemDirectoryA), E(GetWindowsDirectoryA),
    E(SearchPathA), E(MoveFileExA), E(_lopen), E(_lcreat), E(_lclose), E(_lread), E(_lwrite), E(_llseek),
    E(OpenFile), E(ReadProcessMemory), E(WriteProcessMemory), E(VirtualQuery), E(CreateEventA), E(SetEvent),
    E(ResetEvent), E(PulseEvent), E(CreateThread), E(ExitThread), E(ResumeThread), E(SuspendThread),
    E(GetExitCodeThread), E(TerminateThread), E(InterlockedIncrement), E(InterlockedDecrement),
    E(InterlockedExchange), E(IsTextUnicode), E(RegOpenKeyA), E(RegOpenKeyExA), E(RegCreateKeyA), E(RegCreateKeyExA),
    E(RegQueryValueA), E(RegQueryValueExA), E(RegSetValueExA), E(RegDeleteValueA), E(RegEnumKeyA), E(RegCloseKey),
    E(GetProfileStringA), E(GetPrivateProfileStringA), E(GetProfileIntA), E(GetPrivateProfileIntA),
    E(WriteProfileStringA), E(WritePrivateProfileStringA), E(UuidCreate), E(UuidToStringA), E(RpcStringFreeA), E(CLSIDFromString),
    E(RtlInitString), E(RtlInitAnsiString), E(RtlInitUnicodeString), E(RtlAnsiStringToUnicodeString),
    E(RtlUnicodeStringToAnsiString), E(RtlAnsiCharToUnicodeChar), E(RtlFreeUnicodeString), E(RtlFreeAnsiString),
    E(RtlNtStatusToDosError), E(RtlImageNtHeader), E(RtlImageDirectoryEntryToData), E(CheckSumMappedFile),
    E(MapFileAndCheckSumA),
    E(GetCommandLineW), E(CreateFileW), E(GetFileAttributesW), E(SetFileAttributesW),
    E(DeleteFileW), E(CreateDirectoryW), E(RemoveDirectoryW), E(SetCurrentDirectoryW), E(GetDriveTypeW),
    E(MoveFileW), E(SetEnvironmentVariableW), E(GetModuleFileNameW), E(GetCurrentDirectoryW), E(GetFullPathNameW),
    E(FindFirstFileW), E(FindNextFileW), E(CompareStringW), E(GetLocaleInfoW),
    E(CreateEventW), E(CreateMutexW), E(CreateSemaphoreW), E(CreateFileMappingW), E(LoadLibraryW), E(GetModuleHandleW),
    E(InitializeCriticalSectionAndSpinCount), E(SetProcessWorkingSetSize), E(GetUserDefaultUILanguage),
    E(IsDebuggerPresent), E(LoadLibraryExA), E(LoadLibraryExW), E(GetStartupInfoW), E(ReadConsoleInputW), E(CopyFileW),
    E(ReadConsoleW), E(GetVersionExW), E(GetEnvironmentVariableW), E(ExpandEnvironmentStringsW), E(GetTempPathW),
    E(GetTempFileNameW), E(GetShortPathNameW), E(GetLongPathNameW), E(GetComputerNameW), E(GetDiskFreeSpaceExW),
    E(GetFileAttributesExA), E(GetFileAttributesExW), E(FindResourceW), E(LoadStringW), E(FormatMessageW), E(lstrcmpW),
    E(lstrcmpiW), E(lstrcpyW), E(lstrcpynW), E(lstrcatW), E(CharNextW), E(CharUpperW), E(GetDateFormatA),
    E(GetTimeFormatA), E(RegOpenKeyExW), E(RegQueryValueExW), E(OpenProcessToken), E(GetTokenInformation),
    E(CryptAcquireContextA), E(CryptAcquireContextW), E(CryptReleaseContext), E(CryptGenRandom), E(CryptCreateHash),
    E(CryptHashData), E(CryptGetHashParam), E(CryptDestroyHash),
};

u32 api_dispatch(u32 index, const u32 *args)
{
    static int slice;
    u32 a[16], ret;

    if (index >= (u32)napis)
        die("bad thunk %u", index);
    if (!apis[index].fn)
        die("the program called %s, which ntrun does not have (pc=%08x)", apis[index].name, cpu->pc());
    memcpy(a, args, sizeof a);
    if (multi && ++slice >= 64) { /* now and then the other threads get the CPU */
        slice = 0;
        thread_yield();
    }
    cur_api = apis[index].name;
    if (trace_api > 1)
        fprintf(stderr, "[%d] > %s(%x, %x, %x, %x, %x, %x, %x, %x)\n", (int)getpid(), cur_api, a[0], a[1], a[2], a[3], a[4], a[5],
            a[6], a[7]);
    in_api = 1;
    ret = apis[index].fn(a);
    in_api = 0;
    if (trace_api) {
        const char *nm = apis[index].name;
        fprintf(stderr, "[%d] %s(%x, %x, %x, %x, %x, %x) = %x", (int)getpid(), nm, args[0], args[1], args[2], args[3],
            args[4], args[5], ret);
        if (args[0] >= 0x10000 && args[0] < GSIZE && nm[strlen(nm) - 1] == (char)0x41 && (strstr(nm, "File") || strstr(nm, "Library") || strstr(nm, "Directory") || strstr(nm, "Environment")))
            fprintf(stderr, " \"%.200s\"", gstr(args[0]));
        if (!strcmp(nm, "CreateProcessA"))
            fprintf(stderr, " \"%.200s\" \"%.600s\"", args[0] ? gstr(args[0]) : "", args[1] ? gstr(args[1]) : "");
        if (!strcmp(nm, "GetProcAddress") && args[1] >= 0x10000)
            fprintf(stderr, " \"%.100s\"", gstr(args[1]));
        fprintf(stderr, "\n");
    }
    return ret;
}

/* x86: a Windows function takes its arguments off the stack when it returns. The table has
   the bytes for each function (-1: the caller does it). */
static const struct { const char *name; int bytes; } x86args[] = {
#include "x86args.h"
    /* functions of NTDLL: they are not in the import libraries of the SDKs */
    { "RtlInitAnsiString", 8 }, { "RtlInitString", 8 }, { "RtlInitUnicodeString", 8 },
    { "RtlAnsiStringToUnicodeString", 12 }, { "RtlUnicodeStringToAnsiString", 12 },
    { "RtlFreeAnsiString", 4 }, { "RtlFreeUnicodeString", 4 }, { "RtlNtStatusToDosError", 4 },
    { "RtlImageNtHeader", 4 }, { "RtlImageDirectoryEntryToData", 16 }, { "RtlRaiseException", 4 },
    { "RtlAnsiCharToUnicodeChar", 4 }, { "RtlCopyMemory", 12 },
    /* functions of Windows 2000 and later: the import libraries of these SDKs do not have them */
    { "GetLongPathNameW", 12 }, { "GetUserDefaultUILanguage", 0 }, { "InitializeCriticalSectionAndSpinCount", 8 },
};

int api_argbytes(u32 index)
{
    struct api *p = &apis[index];

    if (!p->argb) {
        for (size_t i = 0; i < sizeof x86args / sizeof x86args[0]; i++)
            if (!strcmp(x86args[i].name, p->name)) {
                p->argb = x86args[i].bytes + 2;
                break;
            }
        if (!p->argb) {
            if (!p->fn)
                die("the program called %s, which ntrun does not have (pc=%08x)", p->name, cpu->pc());
            die("the program called %s: ntrun does not know the size of its arguments on x86", p->name);
        }
    }
    return p->argb - 2;
}

/* ---------------- the command processor (cmd.c) ---------------- */

#ifdef _WIN32
static intptr_t spawn_program(int mode, const char *hp, const char *line)
{
    {
        struct envv ev;
        char *text;
        intptr_t r;

        envv_init(&ev);
        /* the environment of the child: only what the command processor has now */
        for (int i = ev.n - 1; i >= 0; i--)
            if (strncmp(ev.v[i], "NTRUN_", 6) && ev.v[i][0] != '=') {
                free(ev.v[i]);
                ev.v[i] = ev.v[--ev.n];
            }
        for (int i = 0; i < ngenv; i++)
            if (genv[i][0] != '=')
                envv_set(&ev, genv[i]);
        envv_set(&ev, "NTRUN_CHILD=1");
        text = malloc(strlen(line) + 16);
        sprintf(text, "NTRUN_CMDLINE=%s", line);
        envv_set(&ev, text);
        free(text);
        envv_unset(&ev, "NTRUN_STD", 9);
        envv_unset(&ev, "NTRUN_HMAP", 10);
        envv_unset(&ev, "NTRUN_RES2", 10);
        r = spawn_ntrun(mode, hp, &ev);
        envv_free(&ev);
        return r;
    }
}

/* the command processor starts a program and goes on; wait_program gives its exit code */
intptr_t start_program(const char *hp, const char *line) { return spawn_program(_P_NOWAIT, hp, line); }

u32 wait_program(intptr_t proc)
{
    DWORD code = 1;

    WaitForSingleObject((HANDLE)proc, INFINITE);
    GetExitCodeProcess((HANDLE)proc, &code);
    CloseHandle((HANDLE)proc);
    return code;
}
#endif

/* Run the program HP (a host path) with the command line LINE in a new ntrun, wait for it,
   and give its exit code. The program gets the environment as it is now. */
u32 run_program(const char *hp, const char *line)
{
#ifdef _WIN32
    intptr_t r = spawn_program(_P_WAIT, hp, line);

    return r == -1 ? 127 : (u32)r;
#else
    pid_t pid;
    int st, ep[2];

    fflush(stdout);
    fflush(stderr);
    exit_pipe(ep);
    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        char *argv[3] = { self_path, (char *)hp, NULL };
        char **names = NULL;
        int n = 0;

        exit_pipe_child(ep);
        /* the environment of the child: only what the command processor has now */
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            if (eq && eq != *e && strncmp(*e, "NTRUN_", 6)) {
                names = realloc(names, sizeof *names * (size_t)(n + 1));
                names[n] = strdup(*e);
                names[n++][eq - *e] = 0;
            }
        }
        for (int i = 0; i < n; i++)
            unsetenv(names[i]);
        for (int i = 0; i < ngenv; i++)
            if (genv[i][0] != '=')
                putenv(genv[i]);
        setenv("NTRUN_CHILD", "1", 1);
        setenv("NTRUN_CMDLINE", line, 1);
        unsetenv("NTRUN_STD");
        unsetenv("NTRUN_HMAP");
        unsetenv("NTRUN_RES2");
        execv(self_path, argv);
        _exit(127);
    }
    if (ep[1] >= 0)
        close(ep[1]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return exit_pipe_code(ep[0], st);
#endif
}

static u32 cmd_main(const char *cmdline)
{
    int fds[3];

    /* our standard handles become file descriptors 0, 1 and 2 */
    for (int i = 0; i < 3; i++) {
        int fd = hfd(std_h[i]);
        fds[i] = fd >= 0 ? dup(fd) : -1;
    }
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0) {
            dup2(fds[i], i);
            close(fds[i]);
        }
    return cmd_shell(cmdline);
}

/* ---------------- start ---------------- */

static void quote_arg(char *out, size_t n, const char *arg)
{
    if (*arg && !strpbrk(arg, " \t\""))
        snprintf(out, n, "%s", arg);
    else
        snprintf(out, n, "\"%s\"", arg);
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    struct sigaction sa;
#endif
    struct module *exe;
    const char *s;
    char buf[PATH_MAX * 2], *cl;
    u32 stack, stacksize = 0x100000, tebr;
    int ai = 1, iscmd, by_name = 0;
    struct stat st0;

    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-a")) trace_api++; /* -a -a: also a line before each call */
        else if (!strcmp(argv[ai], "-t")) trace_insn = 1;
        else break;
        ai++;
    }
    if (getenv("NTRUN_TRACE") && !trace_api)
        trace_api = atoi(getenv("NTRUN_TRACE")) > 1 ? 2 : 1;
    if (ai >= argc) {
        fprintf(stderr, "usage: ntrun [-a] [-t] program [arguments]\n"
            "  runs a Windows NT console program made for MIPS, Alpha, PowerPC or x86\n"
            "  program: a file (tools/CL.EXE), or a name (cl) to find in the folders of NTRUN_PATH\n"
            "  -a  print each Win32 call (-a -a: also before the call)   -t  print each instruction\n"
            "  NTRUN_PATH     the PATH of the program: Windows folders with ; between them\n"
            "  NTRUN_DLLPATH  host folders with DLLs, with " DLLPATH_SEP " between them\n");
        return 2;
    }
#ifdef __APPLE__
    {
        uint32_t n = sizeof self_path;
        char tmp[PATH_MAX];
        if (_NSGetExecutablePath(tmp, &n) != 0 || !realpath(tmp, self_path))
            snprintf(self_path, sizeof self_path, "%s", argv[0]);
    }
#elif defined(_WIN32)
    if (!GetModuleFileNameA(NULL, self_path, sizeof self_path))
        snprintf(self_path, sizeof self_path, "%s", argv[0]);
    for (char *p = self_path; *p; p++)
        if (*p == '\\')
            *p = '/';
    /* files are binary: no CR LF translation, no end of file at ^Z */
    _set_fmode(_O_BINARY);
    for (int i = 0; i < 3; i++)
        _setmode(i, _O_BINARY);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    if (!realpath("/proc/self/exe", self_path))
        snprintf(self_path, sizeof self_path, "%s", argv[0]);
#endif

#ifdef _WIN32
    if (!host_reserve())
        die("cannot reserve the guest address space: error %lu", GetLastError());
    AddVectoredExceptionHandler(1, on_fault);
#else
    G = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED)
        die("cannot reserve the guest address space: %s", strerror(errno));
    if (mprotect(G + 0x10000, GSIZE - 0x10000, PROT_READ | PROT_WRITE) < 0)
        die("mprotect: %s", strerror(errno));
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif

    drives_init();

    /* reserved: the thunks and the MIPS user PCR page */
    rmap[THUNK_BASE / CHUNK] = 1;
    rmap[0x7fff0000 / CHUNK] = 1;
    rmap[0x7ffe0000 / CHUNK] = 1;
#ifdef _WIN32
    /* also the region below the thunks: a walk up the stack reads the word before a return address */
    if (!host_commit(THUNK_BASE - CHUNK, 1) || !host_commit(THUNK_BASE, 1) || !host_commit(0x7fff0000, 1) ||
        !host_commit(0x7ffe0000, 1))
        die("cannot commit the guest memory: error %lu", GetLastError());
#endif
    seh_init();
    apis[0].name = "(return)";
    napis = 1;
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        apis[napis++] = builtin[i];

    /* "cmd.exe" that is not a Windows program (the empty file next to ntrun, or no file):
       ntrun's own small command processor */
    iscmd = 0;
    if (cmd_is_name(argv[ai])) {
        char mz[2] = { 0, 0 };
        int fd = open(argv[ai], O_RDONLY);
        if (fd >= 0) {
            if (read(fd, mz, 2) < 0)
                mz[0] = 0;
            close(fd);
        }
        iscmd = !(mz[0] == 'M' && mz[1] == 'Z');
    }
    if (iscmd) {
        snprintf(exe_hdir, sizeof exe_hdir, ".");
    } else if (realpath(argv[ai], exe_hpath) && stat(exe_hpath, &st0) == 0 && S_ISREG(st0.st_mode)) {
        snprintf(exe_hdir, sizeof exe_hdir, "%s", exe_hpath);
        *strrchr(exe_hdir, '/') = 0;
    } else {
        /* not a file of the host: the name of a program, to find the way Windows does it (PATH) */
        by_name = 1;
        snprintf(exe_hdir, sizeof exe_hdir, ".");
    }

    /* environment */
    if (getenv("NTRUN_CHILD")) {
        for (char **e = environ; *e; e++)
            if (strncmp(*e, "NTRUN_", 6)) {
                genv = realloc(genv, sizeof *genv * (size_t)(ngenv + 2));
                genv[ngenv++] = strdup(*e);
            }
    } else {
        /* When GNU make started ntrun, its variables are in the environment. NMAKE reads
           MAKEFLAGS too and does not know what GNU make put there: the program does not get them. */
        int gmake = getenv("MAKELEVEL") != NULL;
        for (char **e = environ; *e; e++) {
            if (gmake && (!strncmp(*e, "MAKEFLAGS=", 10) || !strncmp(*e, "MFLAGS=", 7) || !strncmp(*e, "MAKELEVEL=", 10) ||
                    !strncmp(*e, "MAKEOVERRIDES=", 14) || !strncmp(*e, "MAKE_TERM", 9)))
                continue;
            /* The variables of the Unix shell and locale mean nothing to a Windows program, and
               some do harm: NMAKE makes a macro of each variable, and makefiles of the SDK
               samples have a macro LANG of their own. */
            if (!strncmp(*e, "LANG=", 5) || !strncmp(*e, "LANGUAGE=", 9) || !strncmp(*e, "LC_", 3) ||
                    !strncmp(*e, "SHELL=", 6) || !strncmp(*e, "SHLVL=", 6) || !strncmp(*e, "PWD=", 4) ||
                    !strncmp(*e, "OLDPWD=", 7) || !strncmp(*e, "_=", 2) || !strncmp(*e, "TERM=", 5))
                continue;
#ifdef _WIN32
            /* the host's own command processor is not a program of the guest: ntrun's is */
            if (!_strnicmp(*e, "COMSPEC=", 8) || !_strnicmp(*e, "PATH=", 5))
                continue;
#endif
            if (strncmp(*e, "NTRUN_", 6) && strncmp(*e, "PATH=", 5) && strncmp(*e, "WINEPATH=", 9)) {
                genv = realloc(genv, sizeof *genv * (size_t)(ngenv + 2));
                genv[ngenv++] = strdup(*e);
            }
        }
        s = getenv("NTRUN_PATH");
        if (!s)
            s = getenv("WINEPATH");
        if (s) {
            env_set("PATH", s);
        } else if (!by_name) {
            to_guest(exe_hdir, buf, sizeof buf);
            env_set("PATH", buf);
        }
        /* the host's folder for temporary files, as a Windows path: TMPDIR for tools that came
           from Unix (the MIPS "cc"), TMP and TEMP for the others when they are not set */
        s = getenv("TMPDIR");
        to_guest(s && *s ? s : "/tmp", buf, sizeof buf);
        if (s && *s)
            env_set("TMPDIR", buf);
        if (!env_get("TMP"))
            env_set("TMP", buf);
        if (!env_get("TEMP"))
            env_set("TEMP", buf);
    }

    if (by_name) {
        if (!find_program(argv[ai], buf, PATH_MAX) || !realpath(buf, exe_hpath))
            die("cannot find %s: it is not a file, and it is not in the folders of NTRUN_PATH", argv[ai]);
        snprintf(exe_hdir, sizeof exe_hdir, "%s", exe_hpath);
        *strrchr(exe_hdir, '/') = 0;
    }

    /* handles */
    if ((s = getenv("NTRUN_HMAP"))) {
        while (*s) {
            u32 h = (u32)strtoul(s, (char **)&s, 10);
            int fd = -1;
            if (*s == ':')
                fd = (int)strtol(s + 1, (char **)&s, 10);
            if (*s == ',')
                s++;
            if (h >= HBASE && fd >= 0 && fcntl(fd, F_GETFD) != -1) {
                hnew_at(h, HT_FILE);
                hget(h)->fd = fd;
            }
        }
    }
    if ((s = getenv("NTRUN_STD"))) {
        sscanf(s, "%u,%u,%u", &std_h[0], &std_h[1], &std_h[2]);
    } else {
        std_h[0] = hfile(0);
        std_h[1] = hfile(1);
        std_h[2] = hfile(2);
    }

    if (!env_get("COMSPEC")) {
        /* nmake looks for the file: an empty cmd.exe next to ntrun stands for our command processor */
        char hp[PATH_MAX], g[PATH_MAX], *sl;
        snprintf(hp, sizeof hp, "%s", self_path);
        if ((sl = strrchr(hp, '/')))
            snprintf(sl + 1, sizeof hp - (size_t)(sl + 1 - hp), "cmd.exe");
        to_guest(hp, g, sizeof g);
        env_set("COMSPEC", g);
    }
    if (iscmd) {
        const char *c = getenv("NTRUN_CMDLINE");
        char *line = NULL;
        if (!c) {
            size_t n = 0, o = 0;
            for (int i = ai; i < argc; i++)
                n += strlen(argv[i]) + 4;
            line = malloc(n + 1);
            for (int i = ai; i < argc; i++) {
                if (i > ai)
                    line[o++] = ' ';
                quote_arg(line + o, n - o, argv[i]);
                o += strlen(line + o);
            }
            line[o] = 0;
            c = line;
        }
        guest_exit(cmd_main(c));
    }

    /* the program */
    s = strrchr(exe_hpath, '/') + 1;
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    exe = load_image(exe_hpath, buf);
    exe = &mods[0];

    /* command line */
    if ((s = getenv("NTRUN_CMDLINE"))) {
        cmdline_g = gdup(s, (u32)strlen(s) + 1);
    } else {
        /* the name of the program is its Windows path: some programs find their other files with it */
        size_t n = PATH_MAX + 4, o = 0;
        char g0[PATH_MAX];
        to_guest(exe_hpath, g0, sizeof g0);
        for (int i = ai + 1; i < argc; i++)
            n += strlen(argv[i]) + 4;
        cl = malloc(n + 1);
        for (int i = ai; i < argc; i++) {
            if (i > ai)
                cl[o++] = ' ';
            quote_arg(cl + o, n - o, i == ai && !by_name ? g0 : argv[i]);
            o += strlen(cl + o);
        }
        cl[o] = 0;
        cmdline_g = gdup(cl, (u32)o + 1);
        free(cl);
    }
    if ((s = getenv("NTRUN_RES2"))) {
        size_t n = strlen(s) / 2;
        u8 *b = malloc(n + 1);
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            sscanf(s + i * 2, "%2x", &v);
            b[i] = (u8)v;
        }
        res2_g = gdup(b, (u32)n);
        res2_len = (u32)n;
        free(b);
    }

    /* stack, TEB and PEB */
    {
        u32 pe = r32(exe->base + 0x3c), opt = exe->base + pe + 24, gp = 0;
        u32 res = r32(opt + 72);
        if (res > stacksize)
            stacksize = (res + CHUNK - 1) & ~(CHUNK - 1);
        stack = region_alloc(0, stacksize);
        tebr = region_alloc(0, CHUNK);
        if (!stack || !tebr)
            die("no room for the stack");
        teb = tebr;
        w32(teb + 4, stack + stacksize);
        w32(teb + 8, stack);
        w32(teb + 0x18, teb);
        w32(teb + 0x20, (u32)getpid());
        w32(teb + 0x24, (u32)getpid() + 1);
        w32(teb + 0x30, teb + 0x2000);
        w32(teb + 0x2000 + 8, exe->base);
        if (r32(opt + 92) > 8 && r32(opt + 96 + 64))
            gp = exe->base + r32(opt + 96 + 64);
        cpu->init(stack + stacksize, teb, gp);
        /* the first thread */
        threads[0].used = 1;
        threads[0].id = (u32)getpid() + 1;
        threads[0].teb = teb;
        threads[0].regs = calloc(1, cpu->state_size);
        exe_gp = gp;
        peb_g = teb + 0x2000;
        stack_default = stacksize;
    }
    exe->inited = 1;
    init_dlls();
    {
        u32 ret = guest_call(exe->entry, 0, NULL);
        u32 b[1] = { ret };
        k_ExitProcess(b);
    }
    return 0;
}
