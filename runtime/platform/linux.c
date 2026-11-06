// Linux host layer (runtime/platform.h), x86-64 and aarch64.
// The guest window is the fixed desktop range (rt.h, 0x7000000000). Write tracking (below) is the copy-mode
// fallback: a page asked about is made read-only, and the first write faults.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <execinfo.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "../platform.h"
#include "../rt.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern char **environ;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }
    return true;
}

// Page write tracking. Used only when the guest heap could not be placed as GPU memory (exec.c). A queried page is
// made read-only; the first write faults (crash_handler -> wt_fault), marks it written, and makes it writable.
// A page never queried counts as written. LARECOMP_NO_WRITE_TRACKING=1 leaves it off. Page size is 4 KB (VC_PAGE).
static uint64_t wt_lo, wt_hi;
static uint8_t *wt_state;  // 0 untracked (counts as written), 1 read-only, 2 written
static atomic_flag wt_spin = ATOMIC_FLAG_INIT;
static bool wt_broken;
static uint64_t wt_odd, wt_odd_addr;
static int wt_odd_repeats;
static void wt_lock(void) { while (atomic_flag_test_and_set_explicit(&wt_spin, memory_order_acquire)) {} }
static void wt_unlock(void) { atomic_flag_clear_explicit(&wt_spin, memory_order_release); }

static bool wt_reopen_all(void) {
    if (mprotect((void *)(uintptr_t)wt_lo, wt_hi - wt_lo, PROT_READ | PROT_WRITE)) return false;
    memset(wt_state, 0, (size_t)((wt_hi - wt_lo) >> 12));
    return true;
}

static bool wt_fault(uint64_t a) {
    if (!wt_state || a < wt_lo || a >= wt_hi) return false;
    uint64_t p = (a - wt_lo) >> 12, np = (wt_hi - wt_lo) >> 12;
    bool ours = false;
    wt_lock();
    if (wt_state[p] == 1) wt_odd_repeats = 0;
    else if (wt_odd_addr == a && ++wt_odd_repeats > 8) { wt_unlock(); return false; }
    else if (wt_odd_addr != a) wt_odd_addr = a, wt_odd_repeats = 0;
    if (wt_state[p] != 1) wt_odd++;
    for (uint64_t k = p; k < p + 2 && k < np; k++) {
        if (k != p && wt_state[k] != 1) continue;
        if (k == p && wt_state[k] == 0) {
            if (mprotect((void *)(uintptr_t)(wt_lo + (k << 12)), 4096, PROT_READ | PROT_WRITE)) {
                if (errno == ENOMEM && wt_reopen_all()) ours = true;
                break;
            }
            ours = true;
            continue;
        }
        wt_state[k] = 2;
        if (mprotect((void *)(uintptr_t)(wt_lo + (k << 12)), 4096, PROT_READ | PROT_WRITE)) {
            ours = wt_reopen_all();
            break;
        }
        ours = true;
    }
    wt_unlock();
    return ours;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    if (wt_state && lo < wt_hi && hi > wt_lo) {
        uint64_t a = lo < wt_lo ? wt_lo : lo, e = hi > wt_hi ? wt_hi : hi;
        wt_lock();
        memset(wt_state + ((a - wt_lo) >> 12), 0, (size_t)((e - a) >> 12));
        wt_unlock();
    }
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

void plat_write_tracking_enable(uint64_t lo, uint64_t hi) {
    if (wt_state || getenv("LARECOMP_NO_WRITE_TRACKING")) return;
    lo &= ~0xFFFull;
    hi = (hi + 0xFFF) & ~0xFFFull;
    uint8_t *s = calloc((size_t)((hi - lo) >> 12), 1);
    if (!s) return;
    wt_lo = lo;
    wt_hi = hi;
    wt_state = s;
    fprintf(stderr, "[linux] page write tracking on %#llx-%#llx\n", (unsigned long long)lo, (unsigned long long)hi);
}

void plat_write_prepare(uint64_t addr, uint64_t size) {
    if (!wt_state || !size || addr + size <= wt_lo || addr >= wt_hi) return;
    uint64_t a = addr < wt_lo ? wt_lo : addr, e = addr + size > wt_hi ? wt_hi : addr + size;
    uint64_t p0 = (a - wt_lo) >> 12, p1 = (e - wt_lo + 0xFFF) >> 12;
    wt_lock();
    for (uint64_t p = p0; p < p1;) {
        if (wt_state[p] != 1) { p++; continue; }
        uint64_t q = p;
        while (q < p1 && wt_state[q] == 1) wt_state[q++] = 2;
        mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ | PROT_WRITE);
        p = q;
    }
    wt_unlock();
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!wt_state || addr < wt_lo || addr + size > wt_hi) return -1;
    *page = 4096;
    uint64_t p0 = (addr - wt_lo) >> 12, p1 = (addr + size - wt_lo + 0xFFF) >> 12;
    int n = 0;
    wt_lock();
    for (uint64_t p = p0; p < p1;) {
        if (wt_state[p] == 1) { p++; continue; }
        uint64_t q = p;
        for (; q < p1 && wt_state[q] != 1; q++) {
            if (n < max) pages[n] = wt_lo + (q << 12);
            n++;
        }
        if (!wt_broken) {
            for (uint64_t k = p; k < q; k++) wt_state[k] = 1;
            if (mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ)) {
                if (errno == ENOMEM && wt_reopen_all()) { wt_unlock(); return -1; }
                for (uint64_t k = p; k < q; k++) wt_state[k] = 0;
                mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ | PROT_WRITE);
                wt_broken = true;
                fprintf(stderr, "[linux] page write tracking: mprotect failed (%d), no new pages tracked (odd faults %llu)\n",
                        errno, (unsigned long long)wt_odd);
            }
        }
        p = q;
    }
    wt_unlock();
    return n > max ? max : n;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
}

void plat_init(void) {}

bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

void plat_terminate(void) { _exit(0); }

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx\n", i, (unsigned long long)pc);
    }
}

void plat_fatal_notice(void) {
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BOTW Recomp",
                             "BOTW Recomp stopped. See the terminal or the log next to the data folder.", NULL);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if (sig == SIGSEGV && wt_fault(fa)) return;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    uint64_t sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;
}

bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
