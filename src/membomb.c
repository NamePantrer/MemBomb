/*
 * MemBomb — учебная демонстрация выделения памяти до мягкого лимита.
 * Пётр Калинкин, Университет ИТМО.
 *
 * По умолчанию режим SAFE: программа берёт не больше DEFAULT_SOFT_MB
 * и никогда не больше HARD_CAP_MB. Безлимитного режима нет.
 * Запускать только в изолированной виртуальной машине.
 * Память освобождается до выхода. ulimit и cgroups программа не обходит.
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HARD_CAP_MB 256u
#define DEFAULT_SOFT_MB 64u
#define DEFAULT_CHUNK_MB 4u
#define DEFAULT_STAGGER_MS 20u
#define MAX_SAMPLES 4096
#define BASELINE_MS 800u
#define HOLD_MS 1000u
#define TAIL_MS 800u
#define INTERVAL_MS 100u

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
static volatile sig_atomic_t g_stop;

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

static unsigned long long mem_kb_field(int which)
{
    MEMORYSTATUSEX s;
    s.dwLength = sizeof(s);
    if (!GlobalMemoryStatusEx(&s))
        return 0;
    if (which == 0)
        return s.ullTotalPhys / 1024ull;
    return s.ullAvailPhys / 1024ull;
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

static void print_alloc_fail(void)
{
    fprintf(stderr,
            "VirtualAlloc не удался, GetLastError=%lu.\n"
            "Лимит ОС (commit charge или квота задания) сработал раньше HARD_CAP.\n",
            (unsigned long)GetLastError());
}

static BOOL WINAPI on_console_ctrl(DWORD ev)
{
    if (ev == CTRL_C_EVENT || ev == CTRL_CLOSE_EVENT || ev == CTRL_SHUTDOWN_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

static void install_stop_handlers(void)
{
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
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
    char line[256];
    char name[64];
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

static unsigned long long mem_kb_field(int which)
{
    if (which == 0)
        return meminfo_kb("MemTotal:");
    if (meminfo_kb("MemAvailable:"))
        return meminfo_kb("MemAvailable:");
    return meminfo_kb("MemFree:");
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

static void print_alloc_fail(void)
{
    fprintf(stderr,
            "mmap не удался: %s.\n"
            "Лимит ОС (ulimit -v или cgroup memory.max) сработал раньше HARD_CAP.\n",
            strerror(errno));
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_stop_handlers(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}

#endif

static double bytes_mb(unsigned long long n)
{
    return (double)n / (1024.0 * 1024.0);
}

static size_t align_down_page(size_t n)
{
    if (g_page == 0)
        return 0;
    return (n / g_page) * g_page;
}

static void touch_pages(void *mem, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)mem;
    size_t i;
    for (i = 0; i < n; i += g_page)
        p[i] = 1;
}

static int keep_chunk(void *p, size_t n)
{
    if (g_nchunks >= g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 16;
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
    g_samples[g_nsamples].free_mb = bytes_mb(mem_kb_field(1) * 1024ull);
    g_samples[g_nsamples].allocated_mb = bytes_mb(g_allocated);
    g_nsamples++;
}

static void sample_for(unsigned ms)
{
    double until = monotonic_sec() + (double)ms / 1000.0;
    do {
        record();
        if (g_stop)
            break;
        sleep_ms(INTERVAL_MS);
    } while (!g_stop && monotonic_sec() < until);
}

static void write_csv(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "t_sec,free_mb,allocated_mb\n");
    for (i = 0; i < g_nsamples; i++) {
        fprintf(f, "%.3f,%.2f,%.2f\n",
                g_samples[i].t_sec, g_samples[i].free_mb, g_samples[i].allocated_mb);
    }
    fclose(f);
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
        out[i] = lo + step * (double)i;
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
    if (x_max <= x_min)
        x_max = x_min + 1.0;
    y_min = y_max = g_samples[0].allocated_mb;
    for (i = 1; i < g_nsamples; i++) {
        if (g_samples[i].allocated_mb < y_min)
            y_min = g_samples[i].allocated_mb;
        if (g_samples[i].allocated_mb > y_max)
            y_max = g_samples[i].allocated_mb;
    }
    pad = (y_max - y_min) * 0.08;
    if (pad < 1.0)
        pad = 1.0;
    y_min -= pad;
    if (y_min < 0)
        y_min = 0;
    y_max += pad;
    if (y_max <= y_min)
        y_max = y_min + 1.0;
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
            "  <text class=\"title\" x=\"88\" y=\"28\">Память, затронутая процессом</text>\n"
            "  <text class=\"caption\" x=\"88\" y=\"46\">учебный прогон, потолок HARD_CAP %u МиБ</text>\n",
            HARD_CAP_MB);
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
        double sy = top + height - (g_samples[i].allocated_mb - y_min) / (y_max - y_min) * height;
        fprintf(f, "%.1f,%.1f ", sx, sy);
    }
    fprintf(f, "\" />\n");
    fprintf(f, "  <text x=\"%.1f\" y=\"455\" text-anchor=\"middle\">t, s</text>\n", left + width / 2.0);
    fprintf(f,
            "  <text x=\"22\" y=\"%.1f\" text-anchor=\"middle\" transform=\"rotate(-90 22 %.1f)\">МиБ</text>\n"
            "</svg>\n",
            top + height / 2.0, top + height / 2.0);
    fclose(f);
}

static void release_all(unsigned stagger_ms)
{
    while (g_nchunks > 0) {
        size_t i = g_nchunks - 1;
        size_t n = g_chunks[i].size;
        free_chunk(g_chunks[i].p, n);
        g_chunks[i].p = NULL;
        if (g_allocated >= n)
            g_allocated -= n;
        else
            g_allocated = 0;
        g_nchunks--;
        record();
        if (stagger_ms && !g_stop)
            sleep_ms(stagger_ms);
    }
}

static void print_banner(unsigned requested_mb, unsigned effective_mb, unsigned chunk_mb, unsigned stagger_ms)
{
    unsigned long long total_kb = mem_kb_field(0);
    unsigned long long avail_kb = mem_kb_field(1);
    printf("=== MemBomb: учебная демонстрация исчерпания памяти ===\n");
    printf("Только изолированная ВМ. Это не инструмент атаки.\n");
    printf("Режим SAFE (единственный). HARD_CAP = %u МиБ, флагом не поднимается.\n", HARD_CAP_MB);
    printf("Запрошено %u МиБ, к выделению %u МиБ, кусок %u МиБ, пауза %u мс.\n",
           requested_mb, effective_mb, chunk_mb, stagger_ms);
    if (total_kb)
        printf("Сейчас в ОС: всего %.0f МиБ, доступно %.0f МиБ.\n",
               bytes_mb(total_kb * 1024ull), bytes_mb(avail_kb * 1024ull));
    printf("Страницы будут затронуты записью, затем вся память освобождается.\n");
    printf("Ctrl+C прерывает цикл и тоже освобождает память.\n\n");
    fflush(stdout);
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "Использование: membomb [-m МИБ] [-c МИБ] [-s МС] [-h]\n"
            "\n"
            "Учебный прогон в своём процессе. Безлимитного режима нет.\n"
            "  -m МИБ   мягкий лимит (по умолчанию %u, потолок HARD_CAP %u)\n"
            "  -c МИБ   размер одного куска (по умолчанию %u)\n"
            "  -s МС    пауза между кусками, миллисекунды (по умолчанию %u, 0 допустим)\n"
            "  -h       эта справка\n"
            "\n"
            "Значение -m выше HARD_CAP обрезается. -m 0 отклоняется.\n"
            "Дополнительно программа не берёт больше половины доступной памяти.\n"
            "Если mmap/VirtualAlloc вернёт ошибку, цикл останавливается сразу:\n"
            "кусок не уменьшается, чтобы «дожать» машину.\n"
            "Запускать только в изолированной ВМ. Защита: ulimit -v и cgroup memory.max.\n",
            DEFAULT_SOFT_MB, HARD_CAP_MB, DEFAULT_CHUNK_MB, DEFAULT_STAGGER_MS);
}

static int parse_u(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s || *end)
        return -1;
    if (v > 1000000ul)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static unsigned clamp_effective(unsigned requested_mb)
{
    unsigned effective = requested_mb;
    unsigned long long avail_kb;
    unsigned half_mb;

    if (effective > HARD_CAP_MB) {
        fprintf(stderr, "Запрошено %u МиБ, обрезано до HARD_CAP (%u МиБ).\n", requested_mb, HARD_CAP_MB);
        effective = HARD_CAP_MB;
    }
    avail_kb = mem_kb_field(1);
    if (avail_kb > 0) {
        if (avail_kb / 2ull < 1024ull) {
            fprintf(stderr,
                    "Доступно %.1f МиБ. Для прогона нужно хотя бы около 2 МиБ свободных. Останов.\n",
                    bytes_mb(avail_kb * 1024ull));
            return 0;
        }
        half_mb = (unsigned)((avail_kb / 2ull) / 1024ull);
        if (effective > half_mb) {
            fprintf(stderr,
                    "Доступно %.0f МиБ. Чтобы не давить машину, лимит снижен до половины: %u МиБ.\n",
                    bytes_mb(avail_kb * 1024ull), half_mb);
            effective = half_mb;
        }
    }
    if (effective > HARD_CAP_MB)
        effective = HARD_CAP_MB;
    return effective;
}

static int run_bounded(unsigned effective_mb, unsigned chunk_mb, unsigned stagger_ms)
{
    unsigned long long limit_bytes;
    size_t chunk;
    int os_stopped = 0;
    int rc = 0;

    limit_bytes = (unsigned long long)effective_mb * 1024ull * 1024ull;
    chunk = (size_t)chunk_mb * 1024ull * 1024ull;
    if (chunk % g_page)
        chunk = (chunk / g_page + 1) * g_page;
    if (chunk < g_page)
        chunk = g_page;
    if ((unsigned long long)chunk > limit_bytes)
        chunk = align_down_page((size_t)limit_bytes);
    if (chunk < g_page) {
        fprintf(stderr, "Лимит меньше одной страницы (%zu байт). Останов.\n", g_page);
        return 1;
    }

    g_t0 = monotonic_sec();
    sample_for(BASELINE_MS);

    while (!g_stop && g_allocated < limit_bytes) {
        size_t n = chunk;
        void *p;
        unsigned long long room = limit_bytes - g_allocated;
        if ((unsigned long long)n > room)
            n = align_down_page((size_t)room);
        if (n < g_page)
            break;
        p = alloc_chunk(n);
        if (!p) {
            print_alloc_fail();
            os_stopped = 1;
            break;
        }
        touch_pages(p, n);
        if (keep_chunk(p, n) != 0) {
            free_chunk(p, n);
            fprintf(stderr, "Не хватило памяти на таблицу кусков. Останов.\n");
            os_stopped = 1;
            break;
        }
        record();
        printf("выделено %7.2f МиБ\n", bytes_mb(g_allocated));
        fflush(stdout);
        if (stagger_ms)
            sleep_ms(stagger_ms);
    }

    {
        unsigned long long peak = g_allocated;
        if (!g_stop && !os_stopped)
            sample_for(HOLD_MS);
        release_all(g_stop ? 0u : stagger_ms);
        if (!g_stop)
            sample_for(TAIL_MS);

        write_csv("free_memory.csv");
        write_svg("free_memory.svg");

        printf("\nИтог: потолок этого прогона %u МиБ, затронуто %.2f МиБ, после освобождения осталось %.2f МиБ.\n",
               effective_mb, bytes_mb(peak), bytes_mb(g_allocated));
    }
    if (g_stop) {
        printf("Прервано сигналом. Память освобождена.\n");
        rc = 130;
    } else if (os_stopped) {
        printf("До мягкого лимита не дошли: остановил отказ ОС. Память освобождена.\n");
        rc = 2;
    } else {
        printf("Мягкий лимит достигнут штатно. Память освобождена.\n");
        rc = 0;
    }
    printf("Ряд: free_memory.csv, график: free_memory.svg (в текущем каталоге).\n");
    return rc;
}

int main(int argc, char **argv)
{
    unsigned requested_mb = DEFAULT_SOFT_MB;
    unsigned chunk_mb = DEFAULT_CHUNK_MB;
    unsigned stagger_ms = DEFAULT_STAGGER_MS;
    unsigned effective_mb;
    int i;
    int rc;

    install_stop_handlers();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "-safe") == 0) {
            printf("Флаг -safe не нужен: безопасный режим и есть режим по умолчанию.\n");
            continue;
        }
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            if (parse_u(argv[++i], &requested_mb) != 0) {
                fprintf(stderr, "Некорректное значение -m.\n");
                usage(stderr);
                return 1;
            }
            if (requested_mb == 0) {
                fprintf(stderr, "-m 0 отклонено: безлимитный режим отключён. Потолок HARD_CAP = %u МиБ.\n",
                        HARD_CAP_MB);
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (parse_u(argv[++i], &chunk_mb) != 0 || chunk_mb == 0) {
                fprintf(stderr, "Некорректное значение -c (нужно целое число МиБ от 1).\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if (parse_u(argv[++i], &stagger_ms) != 0) {
                fprintf(stderr, "Некорректное значение -s.\n");
                return 1;
            }
            continue;
        }
        fprintf(stderr, "Неизвестный аргумент: %s\n", argv[i]);
        usage(stderr);
        return 1;
    }

    if (chunk_mb > HARD_CAP_MB)
        chunk_mb = HARD_CAP_MB;

    g_page = page_size();
    effective_mb = clamp_effective(requested_mb);
    if (effective_mb == 0)
        return 1;
    if (chunk_mb > effective_mb)
        chunk_mb = effective_mb;

    print_banner(requested_mb, effective_mb, chunk_mb, stagger_ms);
    rc = run_bounded(effective_mb, chunk_mb, stagger_ms);
    free(g_chunks);
    g_chunks = NULL;
    return rc;
}
