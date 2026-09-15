/* SPIKE 1 -- disposable. Answers four questions before any architecture is committed:
 *
 *   Q1 does setitimer(ITIMER_PROF)+SIGPROF actually fire on Linux/aarch64?
 *   Q2 does backtrace() return sane frames from inside a signal handler?
 *   Q3 can dladdr() resolve a *static* (internal-linkage) function?   <- the big one
 *   Q4 does the ELF .symtab contain that same static function?
 *
 * Q3/Q4 together decide whether the symbolizer can be 10 lines of dladdr or
 * has to be a real ELF parser. Guessing wrong here is a late rewrite.
 *
 * Built with -O1 -g -fno-omit-frame-pointer -rdynamic.
 * -rdynamic is deliberate: it exports symbols into .dynsym, which is the most
 * *favourable* case for dladdr. If dladdr still cannot see the static function
 * even with -rdynamic, the answer is decisive.
 */
#define _GNU_SOURCE
#include <sys/time.h>
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdatomic.h>

static atomic_int g_ticks = 0;
static void *g_frames[64];
static atomic_int g_nframes = 0;

static void handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    atomic_fetch_add(&g_ticks, 1);
    /* NOT async-signal-safe in general -- that is a finding, not an accident.
     * We are here to observe whether it works, and to learn what it costs. */
    int n = backtrace(g_frames, 64);
    atomic_store(&g_nframes, n);
}

/* internal linkage: the compiler need not emit any dynamic symbol for this. */
static double hot_static_function(int n) {
    double acc = 0;
    for (int i = 1; i < n; i++) acc += 1.0 / (double)i;
    return acc;
}

/* external linkage: dladdr's best case. */
double hot_extern_function(int n);
double hot_extern_function(int n) { return hot_static_function(n); }

int main(void) {
    struct sigaction sa;
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) { perror("sigaction"); return 1; }

    struct itimerval t;
    t.it_interval.tv_sec = 0; t.it_interval.tv_usec = 1000;   /* 1 ms */
    t.it_value = t.it_interval;
    if (setitimer(ITIMER_PROF, &t, NULL) != 0) { perror("setitimer"); return 1; }

    volatile double sink = 0;
    for (int i = 0; i < 3000; i++) sink += hot_extern_function(20000);
    (void)sink;

    struct itimerval off = {{0,0},{0,0}};
    setitimer(ITIMER_PROF, &off, NULL);

    int ticks = atomic_load(&g_ticks), nf = atomic_load(&g_nframes);
    printf("Q1 SIGPROF fired      : ticks=%d  -> %s\n", ticks, ticks > 0 ? "YES" : "NO (FATAL)");
    printf("Q2 backtrace in handler: frames=%d -> %s\n", nf, nf > 0 ? "YES" : "NO (FATAL)");
    if (ticks == 0) return 2;

    printf("\nQ3 dladdr() symbolization of the captured stack:\n");
    int resolved = 0, unresolved = 0, saw_static = 0;
    for (int i = 0; i < nf && i < 12; i++) {
        Dl_info info;
        if (dladdr(g_frames[i], &info) && info.dli_sname) {
            printf("   [%2d] %-18p %s\n", i, g_frames[i], info.dli_sname);
            if (strcmp(info.dli_sname, "hot_static_function") == 0) saw_static = 1;
            resolved++;
        } else {
            printf("   [%2d] %-18p <UNRESOLVED>\n", i, g_frames[i]);
            unresolved++;
        }
    }
    printf("\n   resolved=%d unresolved=%d\n", resolved, unresolved);
    printf("   dladdr saw hot_static_function: %s\n", saw_static ? "YES" : "NO");
    printf("\n   VERDICT: dladdr is %s as the symbolizer\n",
           saw_static ? "SUFFICIENT" : "INSUFFICIENT -> must parse ELF .symtab ourselves");
    return 0;
}
