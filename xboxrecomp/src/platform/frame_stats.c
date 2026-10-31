/* Per-frame cost breakdown (RECOMP_FRAME_STATS=1): see frame_stats.h. */
#include "frame_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <pthread.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <unistd.h>
#endif

static const char *const k_tname[FS_T_N] = { "game", "exec", "rec", "pres" };

static struct {
    int set;
#if defined(__SWITCH__)
    Handle h;
#else
    clockid_t c;
#endif
    uint64_t last;
} s_t[FS_T_N];

static uint64_t s_sum[FS_N], s_max[FS_N];
static int s_on = -1;

int fs_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_FRAME_STATS");
        s_on = e && *e == '1';
    }
    return s_on;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int thread_cpu_ns(int i, uint64_t *ns)
{
    if (!s_t[i].set)
        return 0;
#if defined(__SWITCH__)
    {
        u64 t;
        if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, s_t[i].h, (u64)-1))
            && R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, s_t[i].h, (u64)-1)))
            return 0;
        *ns = t * 625 / 12;                 /* 19.2 MHz ticks */
    }
#else
    {
        struct timespec ts;
        if (clock_gettime(s_t[i].c, &ts))
            return 0;
        *ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
#endif
    return 1;
}

#if defined(__ANDROID__)
static void hint_thread(int which);
#endif

#if defined(__ANDROID__)
/* RECOMP_PROF=1: a sampling profiler without perf events (a user build
 * forbids them). Each fs_thread thread gets a timer on its own CPU clock,
 * 1000 Hz; SIGPROF records the interrupted pc. Every 10 s prof.txt in the
 * current folder (the log's) gets "thread offset count" lines, offsets into
 * libmain.so: llvm-symbolizer on out/android/symbols/libmain.so names them. */
#include <signal.h>
#include <ucontext.h>

#define PROF_N (1u << 17)
static uintptr_t s_prof_pc[FS_T_N][PROF_N];
static volatile uint32_t s_prof_n[FS_T_N];
static __thread int t_prof_which = -1;
static uintptr_t s_prof_base;
static int s_prof_on = -1;

static void prof_sig(int sig, siginfo_t *si, void *ctx)
{
    int w = t_prof_which;
    uint32_t k;
    (void)sig; (void)si;
    if (w < 0)
        return;
    k = s_prof_n[w];
    if (k < PROF_N) {
        s_prof_pc[w][k] = (uintptr_t)((ucontext_t *)ctx)->uc_mcontext.pc;
        s_prof_n[w] = k + 1;
    }
}

static void prof_thread(int which)
{
    struct sigevent sev;
    struct itimerspec its;
    timer_t tm;
    if (s_prof_on < 0) {
        const char *e = getenv("RECOMP_PROF");
        Dl_info di;
        s_prof_on = e && *e == '1';
        if (s_prof_on) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = prof_sig;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigaction(SIGPROF, &sa, NULL);
            if (dladdr((void *)prof_thread, &di))
                s_prof_base = (uintptr_t)di.dli_fbase;
            fprintf(stderr, "[prof] sampling at 1000 Hz of thread CPU time, prof.txt every 10 s\n");
        }
    }
    if (!s_prof_on || which == FS_T_PRES)
        return;
    t_prof_which = which;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = gettid();
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tm))
        return;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000000;
    its.it_value = its.it_interval;
    timer_settime(tm, 0, &its, NULL);
}

static int cmp_up(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static void prof_dump(void)
{
    static uint64_t last;
    static uintptr_t buf[PROF_N];
    uint64_t now = mono_ns();
    FILE *f;
    int w;
    if (s_prof_on <= 0 || now - last < 10000000000ull)
        return;
    last = now;
    if (!(f = fopen("prof.txt", "a")))
        return;
    fprintf(f, "# window\n");
    for (w = 0; w < FS_T_PRES; w++) {
        uint32_t n = s_prof_n[w], i, j;
        memcpy(buf, s_prof_pc[w], n * sizeof buf[0]);
        s_prof_n[w] = 0;
        for (i = 0; i < n; i++)
            buf[i] = buf[i] >= s_prof_base ? buf[i] - s_prof_base : 0;
        qsort(buf, n, sizeof buf[0], cmp_up);
        for (i = 0; i < n; i = j) {
            for (j = i; j < n && buf[j] == buf[i]; j++)
                ;
            fprintf(f, "%s %lx %u\n", k_tname[w], (unsigned long)buf[i], j - i);
        }
    }
    fclose(f);
}
#endif

void fs_thread(int which)
{
    if (which < 0 || which >= FS_T_N)
        return;
#if defined(__ANDROID__)
    hint_thread(which);
    prof_thread(which);
#endif
    if (!fs_on())
        return;
#if defined(__SWITCH__)
    s_t[which].h = threadGetCurHandle();
#else
    if (pthread_getcpuclockid(pthread_self(), &s_t[which].c))
        return;
#endif
    s_t[which].set = 1;
    thread_cpu_ns(which, &s_t[which].last);
}

static uint64_t s_last_gpu;          /* the newest frame's GPU span, for the hint */

void fs_add(int stat, uint64_t ns)
{
    if (stat < 0 || stat >= FS_N)
        return;
    if (stat == FS_GPU)
        s_last_gpu = ns;
    /* ponytail: plain adds from up to two threads; a lost update skews one
     * second's average a little, not worth atomics on the hot path. */
    s_sum[stat] += ns;
    if (ns > s_max[stat])
        s_max[stat] = ns;
}

/* ── Android: ADPF performance hints ──────────────────────────────────
 *
 * Android gives an app no say over CPU or GPU clocks; the governors pick
 * them from load, and a frame that is short of work for a moment makes them
 * step down, after which the next busy frame misses 16.7 ms. A performance
 * hint session (API 33) tells the system the frame budget and, every frame,
 * how long the work took; it raises the clocks of the session's threads to
 * keep inside the budget. From API 35 the report carries the GPU time too
 * (AWorkDuration), which is what lets it raise the GPU clock as well.
 *
 * The threads are the ones fs_thread() names (game, exec, rec, pres). The
 * CPU time reported is the busiest one's own CPU time over the frame (the
 * critical path, waits left out); the GPU time is the renderer's timestamp
 * span (fs_want_gpu turns those on). RECOMP_PERF_HINT=0 turns it off. All
 * symbols come from dlsym, so older Androids just go without. */
#if defined(__ANDROID__)
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration AWorkDuration;

static struct {
    APerformanceHintManager *(*get_manager)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*update_target)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*set_threads)(APerformanceHintSession *, const pid_t *, size_t);
    AWorkDuration *(*wd_create)(void);
    void (*wd_start)(AWorkDuration *, int64_t);
    void (*wd_total)(AWorkDuration *, int64_t);
    void (*wd_cpu)(AWorkDuration *, int64_t);
    void (*wd_gpu)(AWorkDuration *, int64_t);
    int (*report2)(APerformanceHintSession *, AWorkDuration *);
} s_ph;

static int s_hint_on = -1;
static int s_hint_fps = 60;
static pid_t s_hint_tid[FS_T_N];
static clockid_t s_hint_clk[FS_T_N];
static uint64_t s_hint_last[FS_T_N];
static volatile int s_hint_threads_changed;
static APerformanceHintSession *s_hint;
static AWorkDuration *s_hint_wd;

static int hint_on(void)
{
    if (s_hint_on < 0) {
        const char *e = getenv("RECOMP_PERF_HINT");
        void *lib;
        s_hint_on = 0;
        if (e && *e == '0')
            return 0;
        lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib)
            return 0;
#define PH_SYM(field, name) *(void **)&s_ph.field = dlsym(lib, name)
        PH_SYM(get_manager, "APerformanceHint_getManager");
        PH_SYM(create, "APerformanceHint_createSession");
        PH_SYM(update_target, "APerformanceHint_updateTargetWorkDuration");
        PH_SYM(report, "APerformanceHint_reportActualWorkDuration");
        PH_SYM(set_threads, "APerformanceHint_setThreads");
        PH_SYM(wd_create, "AWorkDuration_create");
        PH_SYM(wd_start, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        PH_SYM(wd_total, "AWorkDuration_setActualTotalDurationNanos");
        PH_SYM(wd_cpu, "AWorkDuration_setActualCpuDurationNanos");
        PH_SYM(wd_gpu, "AWorkDuration_setActualGpuDurationNanos");
        PH_SYM(report2, "APerformanceHint_reportActualWorkDuration2");
#undef PH_SYM
        s_hint_on = s_ph.get_manager && s_ph.create && s_ph.report && s_ph.update_target;
        if (!(s_ph.wd_create && s_ph.wd_start && s_ph.wd_total && s_ph.wd_cpu && s_ph.wd_gpu
              && s_ph.report2))
            s_ph.report2 = NULL;
        fprintf(stderr, "[perf] performance hints %s\n", !s_hint_on ? "unavailable (API < 33)"
                : s_ph.report2 ? "on, CPU and GPU" : "on, CPU only (API < 35)");
    }
    return s_hint_on;
}

static void hint_thread(int which)
{
    if (!hint_on() || pthread_getcpuclockid(pthread_self(), &s_hint_clk[which]))
        return;
    s_hint_tid[which] = gettid();
    __atomic_store_n(&s_hint_threads_changed, 1, __ATOMIC_RELEASE);
}

int fs_want_gpu(void)
{
    return fs_on() || hint_on();
}

void fs_target_fps(int fps)
{
    if (fps > 0)
        s_hint_fps = fps;
    if (s_hint)
        s_ph.update_target(s_hint, 1000000000ll / s_hint_fps);
}

/* Once per presented frame. */
static void hint_frame(uint64_t now, uint64_t frame_start)
{
    pid_t tids[FS_T_N];
    size_t n = 0;
    uint64_t cpu = 0, gpu = s_last_gpu, total;
    int i;

    if (!hint_on())
        return;
    if (__atomic_exchange_n(&s_hint_threads_changed, 0, __ATOMIC_ACQ_REL)) {
        for (i = 0; i < FS_T_N; i++)
            if (s_hint_tid[i])
                tids[n++] = s_hint_tid[i];
        if (!n)
            return;
        if (!s_hint) {
            s_hint = s_ph.create(s_ph.get_manager(), tids, n, 1000000000ll / s_hint_fps);
            if (!s_hint) {
                fprintf(stderr, "[perf] no hint session (the system refused it)\n");
                s_hint_on = 0;
                return;
            }
            if (s_ph.report2)
                s_hint_wd = s_ph.wd_create();
            fprintf(stderr, "[perf] hint session: %zu threads, %d fps target\n", n, s_hint_fps);
        } else if (s_ph.set_threads) {
            s_ph.set_threads(s_hint, tids, n);
        }
    }
    if (!s_hint)
        return;
    for (i = 0; i < FS_T_N; i++) {
        struct timespec ts;
        uint64_t t;
        if (!s_hint_tid[i] || clock_gettime(s_hint_clk[i], &ts))
            continue;
        t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (s_hint_last[i] && t - s_hint_last[i] > cpu)
            cpu = t - s_hint_last[i];
        s_hint_last[i] = t;
    }
    if (!cpu)
        return;
    total = cpu > gpu ? cpu : gpu;
    if (s_hint_wd && gpu) {
        s_ph.wd_start(s_hint_wd, (int64_t)frame_start);
        s_ph.wd_total(s_hint_wd, (int64_t)total);
        s_ph.wd_cpu(s_hint_wd, (int64_t)cpu);
        s_ph.wd_gpu(s_hint_wd, (int64_t)gpu);
        s_ph.report2(s_hint, s_hint_wd);
    } else {
        s_ph.report(s_hint, (int64_t)cpu);
    }
}
#else
int fs_want_gpu(void) { return fs_on(); }
void fs_target_fps(int fps) { (void)fps; }
#endif

void fs_frame(unsigned draws, unsigned programs, unsigned textures)
{
    static uint64_t window, last, fmax, fsum;
    static unsigned n, ndraws, nprogs, ntex;
    uint64_t now;
    char line[512];
    int len, i;

#if defined(__ANDROID__)
    {
        static uint64_t hint_last;
        uint64_t t = mono_ns();
        if (hint_last)
            hint_frame(t, hint_last);
        hint_last = t;
        prof_dump();
    }
#endif
    if (!fs_on())
        return;
    now = mono_ns();
    if (!window) {
        window = last = now;
        memset(s_sum, 0, sizeof s_sum);
        memset(s_max, 0, sizeof s_max);
        return;
    }
    if (now - last > fmax)
        fmax = now - last;
    fsum += now - last;
    last = now;
    n++;
    ndraws += draws;
    nprogs += programs;
    ntex += textures;
    if (now - window < 1000000000ull)
        return;

    len = snprintf(line, sizeof line, "[ft] %.0f fps, frame %.1f avg / %.1f max ms | cpu per frame:",
                   n * 1e9 / (double)(now - window), fsum / 1e6 / n, fmax / 1e6);
    for (i = 0; i < FS_T_N; i++) {
        uint64_t t;
        if (!thread_cpu_ns(i, &t))
            continue;
        len += snprintf(line + len, sizeof line - len, " %s %.1f", k_tname[i],
                        (t - s_t[i].last) / 1e6 / n);
        s_t[i].last = t;
    }
    len += snprintf(line + len, sizeof line - len,
                    " | game fence wait %.1f / %.1f | gpu %.1f / %.1f"
                    " | rec slot wait %.1f, acquire %.1f | %u draws, %u programs, %u textures",
                    s_sum[FS_GAME_WAIT] / 1e6 / n, s_max[FS_GAME_WAIT] / 1e6,
                    s_sum[FS_GPU] / 1e6 / n, s_max[FS_GPU] / 1e6,
                    s_sum[FS_SLOT_WAIT] / 1e6 / n, s_sum[FS_ACQUIRE] / 1e6 / n,
                    ndraws / n, nprogs, ntex);
    fprintf(stderr, "%s\n", line);
    memset(s_sum, 0, sizeof s_sum);
    memset(s_max, 0, sizeof s_max);
    window = now;
    fmax = fsum = 0;
    n = ndraws = nprogs = ntex = 0;
}
