#ifndef _WIN32
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SAMPLES 16384
#define DEFAULT_SAFE_MB 1024
#define DEFAULT_CHUNK_MB 4
#define DEFAULT_STAGGER_MS 10
#define BASELINE_MS 1500
#define HOLD_MS 2000
#define TAIL_MS 2000
#define INTERVAL_MS 100
#define CRASH_BASELINE_MS 400
#define CRASH_INTERVAL_MS 50
#define CRASH_LOG "last_moments.csv"
#define CRASH_SVG "last_moments.svg"
#define HEARTBEAT "last_heartbeat.txt"
#ifdef MEMBOMB_RAW
#define NO_FILES 1
#else
#define NO_FILES 0
#endif

typedef struct {
    void *p;
    size_t size;
} Chunk;

typedef struct {
    double t_sec;
    double free_mb;
    double allocated_mb;
} Sample;

static Sample g_samples[MAX_SAMPLES];
static int g_nsamples;
static double g_t0;
static Chunk *g_chunks;
static size_t g_nchunks;
static size_t g_cap;
static unsigned long long g_allocated;
static size_t g_page;
static FILE *g_log;
static volatile int g_stop;
static int g_persist_ok = 1;
static const char *g_chart_title = "free memory";

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>

static double monotonic_sec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

static void sleep_ms(unsigned ms)
{
    Sleep(ms);
}

static size_t page_size(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
}

static unsigned long long phys_total_bytes(void)
{
    MEMORYSTATUSEX s;
    s.dwLength = sizeof(s);
    if (!GlobalMemoryStatusEx(&s))
        return 0;
    return s.ullTotalPhys;
}

static unsigned long long phys_free_bytes(void)
{
    MEMORYSTATUSEX s;
    s.dwLength = sizeof(s);
    if (!GlobalMemoryStatusEx(&s))
        return 0;
    return s.ullAvailPhys;
}

static void *alloc_chunk(size_t n)
{
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static void free_chunk(void *p, size_t n)
{
    (void)n;
    if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

static void print_alloc_fail(size_t tried)
{
    (void)tried;
    printf("VirtualAlloc failed (%lu)\n", (unsigned long)GetLastError());
}

static int set_memory_quota_mb(unsigned mb)
{
    HANDLE job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    job = CreateJobObjectA(NULL, NULL);
    if (!job)
        return -1;
    memset(&lim, 0, sizeof(lim));
    lim.ProcessMemoryLimit = (SIZE_T)mb * 1024ull * 1024ull;
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim)))
        return -1;
    if (!AssignProcessToJobObject(job, GetCurrentProcess()))
        return -1;
    return 0;
}

static void harden_flush(FILE *f)
{
    fflush(f);
    if (f)
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
}

static BOOL WINAPI on_console_ctrl(DWORD ev)
{
    if (ev == CTRL_C_EVENT || ev == CTRL_CLOSE_EVENT || ev == CTRL_SHUTDOWN_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

#else

#include <sys/mman.h>
#include <unistd.h>

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static size_t page_size(void)
{
    long n = sysconf(_SC_PAGESIZE);
    if (n <= 0)
        return 4096;
    return (size_t)n;
}

static unsigned long long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256], name[64];
    unsigned long long val;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %llu", name, &val) == 2 && strcmp(name, key) == 0) {
            fclose(f);
            return val;
        }
    }
    fclose(f);
    return 0;
}

static unsigned long long phys_total_bytes(void)
{
    return meminfo_kb("MemTotal:") * 1024ull;
}

static unsigned long long phys_free_bytes(void)
{
    unsigned long long avail = meminfo_kb("MemAvailable:");
    if (avail)
        return avail * 1024ull;
    return meminfo_kb("MemFree:") * 1024ull;
}

static void *alloc_chunk(size_t n)
{
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return p;
}

static void free_chunk(void *p, size_t n)
{
    if (p && n)
        munmap(p, n);
}

static void print_alloc_fail(size_t tried)
{
    (void)tried;
    perror("mmap");
}

static int set_memory_quota_mb(unsigned mb)
{
    (void)mb;
    return -1;
}

static void harden_flush(FILE *f)
{
    fflush(f);
    if (f)
        fsync(fileno(f));
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

#endif

static double bytes_mb(unsigned long long n)
{
    return (double)n / (1024.0 * 1024.0);
}

static void touch_pages(void *mem, size_t n)
{
    unsigned char *p = (unsigned char *)mem;
    size_t i;
    for (i = 0; i < n; i += g_page)
        *(volatile unsigned char *)(p + i) = 0;
}

static int keep_chunk(void *p, size_t n)
{
    if (g_nchunks >= g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 64;
        Chunk *nw = (Chunk *)realloc(g_chunks, ncap * sizeof(Chunk));
        if (!nw)
            return -1;
        g_chunks = nw;
        g_cap = ncap;
    }
    g_chunks[g_nchunks].p = p;
    g_chunks[g_nchunks].size = n;
    g_nchunks++;
    g_allocated += n;
    return 0;
}

static void record(void)
{
    if (g_nsamples >= MAX_SAMPLES)
        return;
    g_samples[g_nsamples].t_sec = monotonic_sec() - g_t0;
    g_samples[g_nsamples].free_mb = bytes_mb(phys_free_bytes());
    g_samples[g_nsamples].allocated_mb = bytes_mb(g_allocated);
    g_nsamples++;
}

static int persist_last(int durable)
{
    FILE *hb;
    const Sample *s;
    if (NO_FILES)
        return 0;
    if (g_nsamples < 1)
        return 0;
    s = &g_samples[g_nsamples - 1];
    if (g_log) {
        if (fprintf(g_log, "%.3f,%.2f,%.2f\n", s->t_sec, s->free_mb, s->allocated_mb) < 0) {
            g_persist_ok = 0;
            return -1;
        }
        if (durable)
            harden_flush(g_log);
        else
            fflush(g_log);
    }
    hb = fopen(HEARTBEAT, "w");
    if (hb) {
        fprintf(hb, "t_sec=%.3f free_mb=%.2f allocated_mb=%.2f chunks=%zu persist=%s\n",
                s->t_sec, s->free_mb, s->allocated_mb, g_nchunks,
                g_persist_ok ? "ok" : "fail");
        if (durable)
            harden_flush(hb);
        else
            fflush(hb);
        fclose(hb);
    }
    return 0;
}

static void sample_now(int durable)
{
    record();
    persist_last(durable);
}

static void sample_for(unsigned ms, unsigned interval_ms, int durable)
{
    double until = monotonic_sec() + ms / 1000.0;
    do {
        sample_now(durable);
        sleep_ms(interval_ms);
    } while (!g_stop && monotonic_sec() < until);
}

static size_t align_page(size_t n)
{
    if (n < g_page)
        return g_page;
    return (n / g_page) * g_page;
}

static size_t next_chunk(size_t last, int fixed_mb)
{
    size_t c;
    if (last == 0) {
        if (fixed_mb > 0)
            c = (size_t)fixed_mb * 1024ull * 1024ull;
        else
            c = g_page;
    } else if (last > (size_t)-1 / 2) {
        c = last;
    } else {
        c = last * 2;
    }
    return align_page(c);
}

static void write_csv(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f)
        return;
    fprintf(f, "t_sec,free_mb,allocated_mb\n");
    for (i = 0; i < g_nsamples; i++)
        fprintf(f, "%.3f,%.2f,%.2f\n",
                g_samples[i].t_sec, g_samples[i].free_mb, g_samples[i].allocated_mb);
    fclose(f);
}

static int load_csv(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    Sample all[MAX_SAMPLES];
    int n = 0, i;
    if (!f)
        return -1;
    g_nsamples = 0;
    while (fgets(line, sizeof(line), f)) {
        Sample s;
        if (line[0] == '#' || strncmp(line, "t_sec", 5) == 0)
            continue;
        if (sscanf(line, "%lf,%lf,%lf", &s.t_sec, &s.free_mb, &s.allocated_mb) != 3)
            continue;
        if (n < MAX_SAMPLES)
            all[n++] = s;
        else {
            memmove(all + 200, all + 201, (MAX_SAMPLES - 201) * sizeof(Sample));
            all[MAX_SAMPLES - 1] = s;
        }
    }
    fclose(f);
    if (n < 1)
        return -1;
    for (i = 0; i < n; i++)
        g_samples[i] = all[i];
    g_nsamples = n;
    return 0;
}

static void ticks(double lo, double hi, int n, double *out)
{
    int i;
    double step;
    if (n < 2 || hi == lo) {
        out[0] = lo;
        return;
    }
    step = (hi - lo) / (double)(n - 1);
    for (i = 0; i < n; i++)
        out[i] = lo + step * i;
}

static void write_svg(const char *path)
{
    FILE *f;
    int i;
    double left = 88, top = 64, width = 740, height = 340;
    double x_min, x_max, y_min, y_max, pad;
    double xt[6], yt[6];
    if (g_nsamples < 1)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    x_min = g_samples[0].t_sec;
    x_max = g_samples[g_nsamples - 1].t_sec;
    if (x_max == x_min)
        x_max = x_min + 1.0;
    y_min = y_max = g_samples[0].free_mb;
    for (i = 1; i < g_nsamples; i++) {
        if (g_samples[i].free_mb < y_min)
            y_min = g_samples[i].free_mb;
        if (g_samples[i].free_mb > y_max)
            y_max = g_samples[i].free_mb;
    }
    pad = (y_max - y_min) * 0.08;
    if (pad < 8)
        pad = 8;
    y_min -= pad;
    if (y_min < 0)
        y_min = 0;
    y_max += pad;
    ticks(x_min, x_max, 6, xt);
    ticks(y_min, y_max, 6, yt);
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"880\" height=\"480\" viewBox=\"0 0 880 480\">\n"
            "  <style>\n"
            "    text { font-family: Segoe UI, sans-serif; fill: #1a1a1a; font-size: 12px; }\n"
            "    .title { font-size: 16px; font-weight: 600; }\n"
            "    .caption { font-size: 11px; fill: #555; }\n"
            "    line { stroke: #ddd; stroke-width: 1; }\n"
            "    .frame { stroke: #333; fill: none; }\n"
            "    .series { fill: none; stroke: #1d4ed8; stroke-width: 2; }\n"
            "  </style>\n"
            "  <text class=\"title\" x=\"88\" y=\"28\">%s</text>\n"
            "  <text class=\"caption\" x=\"88\" y=\"46\">%s</text>\n",
            g_chart_title, path);
    for (i = 0; i < 6; i++) {
        double gy = top + height - (yt[i] - y_min) / (y_max - y_min) * height;
        double gx = left + (xt[i] - x_min) / (x_max - x_min) * width;
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" />\n",
                left, gy, left + width, gy);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" />\n",
                gx, top, gx, top + height);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" text-anchor=\"end\">%.0f</text>\n",
                left - 8, gy + 4, yt[i]);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" text-anchor=\"middle\">%.1f</text>\n",
                gx, top + height + 18, xt[i]);
    }
    fprintf(f, "  <rect class=\"frame\" x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" />\n",
            left, top, width, height);
    fprintf(f, "  <polyline class=\"series\" points=\"");
    for (i = 0; i < g_nsamples; i++) {
        double sx = left + (g_samples[i].t_sec - x_min) / (x_max - x_min) * width;
        double sy = top + height - (g_samples[i].free_mb - y_min) / (y_max - y_min) * height;
        fprintf(f, "%.1f,%.1f ", sx, sy);
    }
    fprintf(f, "\" />\n");
    fprintf(f, "  <text x=\"%.1f\" y=\"455\" text-anchor=\"middle\">t, s</text>\n",
            left + width / 2);
    fprintf(f,
            "  <text x=\"22\" y=\"%.1f\" text-anchor=\"middle\" transform=\"rotate(-90 22 %.1f)\">MB</text>\n"
            "</svg>\n",
            top + height / 2, top + height / 2);
    fclose(f);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void archive_previous_crash(void)
{
    char name[160];
    time_t now;
    struct tm *tmv;
    if (NO_FILES || !file_exists(CRASH_LOG))
        return;
    now = time(NULL);
    tmv = localtime(&now);
    if (!tmv || strftime(name, sizeof(name), "last_moments_%Y%m%d_%H%M%S.csv", tmv) == 0)
        snprintf(name, sizeof(name), "last_moments_prev.csv");
    if (rename(CRASH_LOG, name) != 0)
        return;
    if (load_csv(name) == 0)
        write_svg(CRASH_SVG);
    g_nsamples = 0;
}

static void release_all(unsigned stagger_ms)
{
    size_t i = g_nchunks;
    while (i > 0) {
        i--;
        free_chunk(g_chunks[i].p, g_chunks[i].size);
        g_allocated -= g_chunks[i].size;
        g_chunks[i].p = NULL;
        record();
        if (stagger_ms)
            sleep_ms(stagger_ms);
    }
    g_nchunks = 0;
}

static void install_stop_handlers(void)
{
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
}

static int open_crash_log(unsigned long long total, unsigned long long avail)
{
    (void)total;
    (void)avail;
    if (NO_FILES)
        return 0;
    g_log = fopen(CRASH_LOG, "w");
    if (!g_log)
        return -1;
#if defined(_WIN32)
    setvbuf(g_log, NULL, _IONBF, 0);
#else
    setvbuf(g_log, NULL, _IOLBF, 0);
#endif
    fprintf(g_log, "t_sec,free_mb,allocated_mb\n");
    return 0;
}

static void close_crash_log(void)
{
    if (!g_log)
        return;
    harden_flush(g_log);
    fclose(g_log);
    g_log = NULL;
}

static void deathwatch(unsigned ms)
{
    sample_for(ms, 40, 1);
}

static int run_crash(int chunk_mb, unsigned stagger_ms, unsigned quota_mb)
{
    unsigned long long total = phys_total_bytes();
    unsigned long long avail = phys_free_bytes();
    size_t this_chunk;
    int failed = 0;
    unsigned long long n = 0;

    archive_previous_crash();
    if (open_crash_log(total, avail) != 0)
        return 1;

    if (quota_mb) {
        if (set_memory_quota_mb(quota_mb) != 0) {
            close_crash_log();
            return 1;
        }
    }

    g_t0 = monotonic_sec();
    if (!NO_FILES)
        sample_for(CRASH_BASELINE_MS, CRASH_INTERVAL_MS, 1);

    this_chunk = next_chunk(0, chunk_mb);
    while (!g_stop) {
        unsigned long long free_now = phys_free_bytes();
        int durable;
        void *p;
        p = alloc_chunk(this_chunk);
        if (!p && this_chunk > g_page) {
            this_chunk = align_page(this_chunk / 2);
            continue;
        }
        if (!p) {
            print_alloc_fail(this_chunk);
            failed = 1;
            break;
        }
        touch_pages(p, this_chunk);
        if (keep_chunk(p, this_chunk) != 0) {
            free_chunk(p, this_chunk);
            failed = 1;
            break;
        }
        durable = (free_now < 256ull * 1024ull * 1024ull) || ((n & 7ull) == 0);
        sample_now(durable);
        n++;
        this_chunk = next_chunk(this_chunk, 0);
        if (stagger_ms)
            sleep_ms(stagger_ms);
        if (!g_persist_ok)
            break;
    }

    if (!NO_FILES && !g_stop)
        deathwatch(failed ? 8000 : 4000);

    if (!NO_FILES) {
        write_svg(CRASH_SVG);
        close_crash_log();
    } else if (failed) {
        while (!g_stop)
            sleep_ms(1000);
    }
    return failed ? 2 : 0;
}

static int run_safe(unsigned long long max_bytes, size_t chunk, unsigned stagger_ms, unsigned quota_mb)
{
    int failed = 0;

    if (quota_mb) {
        if (set_memory_quota_mb(quota_mb) != 0)
            return 1;
        if (max_bytes == 0 || max_bytes > (unsigned long long)quota_mb * 1024ull * 1024ull)
            max_bytes = 0;
    }

    g_t0 = monotonic_sec();
    sample_for(BASELINE_MS, INTERVAL_MS, 0);

    {
        size_t this_chunk = next_chunk(0, 0);
        if (chunk > g_page)
            this_chunk = align_page(chunk);
        for (;;) {
            void *p;
            if (g_stop)
                break;
            if (max_bytes > 0) {
                if (g_allocated >= max_bytes)
                    break;
                if (g_allocated + this_chunk > max_bytes)
                    this_chunk = align_page((size_t)(max_bytes - g_allocated));
                if (this_chunk < g_page || g_allocated + this_chunk > max_bytes)
                    break;
            }
            p = alloc_chunk(this_chunk);
            if (!p && this_chunk > g_page) {
                this_chunk = align_page(this_chunk / 2);
                continue;
            }
            if (!p) {
                print_alloc_fail(this_chunk);
                failed = 1;
                break;
            }
            touch_pages(p, this_chunk);
            if (keep_chunk(p, this_chunk) != 0) {
                free_chunk(p, this_chunk);
                failed = 1;
                break;
            }
            record();
            this_chunk = next_chunk(this_chunk, 0);
            if (stagger_ms)
                sleep_ms(stagger_ms);
        }
    }

    sample_for(HOLD_MS, INTERVAL_MS, 0);
    release_all(stagger_ms);
    sample_for(TAIL_MS, INTERVAL_MS, 0);
    if (!NO_FILES) {
        g_chart_title = "free memory";
        write_csv("free_memory.csv");
        write_svg("free_memory.svg");
    }
    return failed ? 2 : 0;
}

int main(int argc, char **argv)
{
    unsigned long long max_bytes = (unsigned long long)DEFAULT_SAFE_MB * 1024ull * 1024ull;
    size_t chunk = (size_t)DEFAULT_CHUNK_MB * 1024ull * 1024ull;
    unsigned stagger_ms = 0;
    unsigned quota_mb = 0;
    int safe = 0;
    int plot_only = 0;
    int chunk_mb = 0;
    int i;
    int rc;

    install_stop_handlers();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-plot") == 0)
            plot_only = 1;
        else if (strcmp(argv[i], "-safe") == 0)
            safe = 1;
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            int mb = atoi(argv[++i]);
            if (mb < 0)
                mb = 0;
            max_bytes = (unsigned long long)mb * 1024ull * 1024ull;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            chunk_mb = atoi(argv[++i]);
            if (chunk_mb < 1)
                chunk_mb = 1;
            chunk = (size_t)chunk_mb * 1024ull * 1024ull;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            int ms = atoi(argv[++i]);
            if (ms < 0)
                ms = 0;
            stagger_ms = (unsigned)ms;
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            int mb = atoi(argv[++i]);
            if (mb < 1)
                mb = 1;
            quota_mb = (unsigned)mb;
        }
    }

    g_page = page_size();
    if (chunk % g_page)
        chunk = (chunk / g_page + 1) * g_page;

    if (plot_only) {
        const char *src;
        if (NO_FILES)
            return 1;
        src = file_exists(CRASH_LOG) ? CRASH_LOG : NULL;
        if (!src || load_csv(src) != 0)
            return 1;
        write_svg(CRASH_SVG);
        return 0;
    }

    if (safe)
        rc = run_safe(max_bytes, chunk, stagger_ms ? stagger_ms : DEFAULT_STAGGER_MS, quota_mb);
    else
        rc = run_crash(chunk_mb, stagger_ms, quota_mb);

    free(g_chunks);
    return rc;
}
