/* OSTEP ch.6 homework, part 1: how much does a system call cost?
 *
 * Plan (in order, matching the measurement methodology):
 *   [A] Compare three timers first — gettimeofday (µs, vDSO),
 *       clock_gettime(CLOCK_MONOTONIC) (ns, vDSO), and raw rdtsc (cycles) —
 *       measuring each one's resolution (smallest observable tick) and its
 *       own per-call cost. This tells you whether the timer is precise
 *       enough before trusting any measurement made with it.
 *   [B] Time N iterations of the cheapest possible syscall, a 0-byte
 *       read(2), sweeping N = 10^3 .. 10^max_exp. Watching the per-call
 *       estimate converge as N grows shows how many iterations a
 *       meaningful measurement needs.
 *
 * usage: ./syscall_cost [max_exp] [-r]   (default max_exp = 7)
 *   -r : skip the timer comparison, run only the read sweep — useful when
 *        watching under cvisor so the trace starts at the loop of interest
 *
 * NOTE: run natively on x86-64 Linux for numbers that mean anything.
 * Inside an emulated VM (QEMU TCG) or under cvisor the absolute values
 * reflect emulation/tracing overhead, not hardware. Use cvisor only to
 * *verify the mechanism* (e.g. that every loop iteration really enters
 * the kernel — watch the `s` panel), with a small max_exp like 3.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RES_SAMPLES  10000   /* back-to-back call pairs for resolution */
#define COST_ITERS   200000  /* calls for measuring a timer's own cost */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rdtsc(void)
{
    unsigned lo, hi;
    /* lfence keeps earlier instructions from drifting past the read */
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---------------- [A] timer comparison ---------------- */

static void timer_gettimeofday(void)
{
    struct timeval a, b;
    uint64_t min_tick = UINT64_MAX;
    int zeros = 0;
    for (int i = 0; i < RES_SAMPLES; i++) {
        gettimeofday(&a, NULL);
        gettimeofday(&b, NULL);
        uint64_t d = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000ull
                     + (uint64_t)(b.tv_usec - a.tv_usec);
        if (d == 0)
            zeros++;
        else if (d < min_tick)
            min_tick = d;
    }

    uint64_t t0 = now_ns();
    for (int i = 0; i < COST_ITERS; i++)
        gettimeofday(&a, NULL);
    uint64_t cost = (now_ns() - t0) / COST_ITERS;

    printf("  gettimeofday   resolution: %llu us tick "
           "(%d/%d consecutive pairs identical)  cost: ~%llu ns/call\n",
           (unsigned long long)(min_tick == UINT64_MAX ? 0 : min_tick),
           zeros, RES_SAMPLES, (unsigned long long)cost);
}

static void timer_clock_gettime(void)
{
    uint64_t min_tick = UINT64_MAX;
    int zeros = 0;
    for (int i = 0; i < RES_SAMPLES; i++) {
        uint64_t a = now_ns(), b = now_ns();
        uint64_t d = b - a;
        if (d == 0)
            zeros++;
        else if (d < min_tick)
            min_tick = d;
    }

    struct timespec ts;
    uint64_t t0 = now_ns();
    for (int i = 0; i < COST_ITERS; i++)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t cost = (now_ns() - t0) / COST_ITERS;

    printf("  clock_gettime  resolution: %llu ns tick "
           "(%d/%d pairs identical)         cost: ~%llu ns/call\n",
           (unsigned long long)(min_tick == UINT64_MAX ? 0 : min_tick),
           zeros, RES_SAMPLES, (unsigned long long)cost);
}

/* estimate the TSC frequency so cycle counts can be converted to time */
static double timer_rdtsc(void)
{
    uint64_t min_tick = UINT64_MAX;
    for (int i = 0; i < RES_SAMPLES; i++) {
        uint64_t a = rdtsc(), b = rdtsc();
        if (b - a > 0 && b - a < min_tick)
            min_tick = b - a;
    }

    uint64_t t0 = now_ns(), c0 = rdtsc();
    while (now_ns() - t0 < 200000000ull) /* busy-wait ~200 ms */
        ;
    uint64_t c1 = rdtsc(), t1 = now_ns();
    double ghz = (double)(c1 - c0) / (double)(t1 - t0);

    printf("  rdtsc          resolution: %llu cycles between reads   "
           "TSC freq: ~%.3f GHz\n",
           (unsigned long long)min_tick, ghz);
    printf("                 (trustworthy only with constant_tsc/nonstop_tsc"
           " in /proc/cpuinfo, and not under emulation)\n");
    return ghz;
}

/* ---------------- [B] 0-byte read sweep ---------------- */

static void sweep_read(int max_exp, double tsc_ghz)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/null");
        exit(1);
    }
    char buf[1];
    volatile long sink = 0; /* keep the loop honest */

    printf("\n[B] 0-byte read(2) cost vs. iteration count\n");
    printf("  %10s  %12s  %10s  %10s\n",
           "N", "total ms", "ns/call", "cycles/call");
    for (int e = 3; e <= max_exp; e++) {
        long n = 1;
        for (int k = 0; k < e; k++)
            n *= 10;

        uint64_t t0 = now_ns();
        for (long i = 0; i < n; i++)
            sink += read(fd, buf, 0);
        uint64_t dt = now_ns() - t0;

        double per = (double)dt / (double)n;
        printf("  %10ld  %12.3f  %10.1f  %10.0f\n",
               n, (double)dt / 1e6, per, per * tsc_ghz);
    }
    close(fd);
    (void)sink;

    printf("\n  -> the per-call estimate is meaningful once the total time\n"
           "     is far above the timer tick and the value stops moving\n"
           "     between rows (pick that N; smaller N = timer noise).\n");
}

int main(int argc, char **argv)
{
    int max_exp = 7, only_read = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            only_read = 1;
        } else {
            max_exp = atoi(argv[i]);
            if (max_exp < 3 || max_exp > 9) {
                fprintf(stderr, "usage: %s [max_exp 3..9] [-r]\n", argv[0]);
                return 2;
            }
        }
    }

    double ghz = 0.0;
    if (!only_read) {
        printf("[A] timer resolution and cost (%d sample pairs each)\n",
               RES_SAMPLES);
        timer_gettimeofday();
        timer_clock_gettime();
        ghz = timer_rdtsc();
    }

    sweep_read(max_exp, ghz);
    return 0;
}
