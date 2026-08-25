/* OSTEP ch.6 homework, part 2: how much does a context switch cost?
 *
 * Two processes pinned to the same CPU, connected by two pipes:
 *
 *   parent: write(pipe1) -> read(pipe2) blocks -> OS switches to child
 *   child : read(pipe1)  -> write(pipe2)       -> blocks on pipe1 again
 *
 * Each round trip therefore forces (at least) two context switches plus
 * four pipe syscalls. Timing many round trips gives:
 *
 *   raw estimate      = round_trip / 2
 *   corrected estimate = (round_trip - 4 * syscall_cost) / 2
 *
 * where syscall_cost is a 0-byte read measured in-process right before the
 * ping-pong. The correction is itself an approximation: a 1-byte pipe
 * read/write does more work than a 0-byte /dev/null read, so the true
 * switch cost lies between the two estimates.
 *
 * usage: ./ctx_switch [cpu] [round_trips]   (default: cpu 0, 50000 trips)
 *
 * NOTE: like syscall_cost, absolute numbers are only meaningful on native
 * x86-64 Linux, not under emulation or cvisor. Under cvisor (small trip
 * count!) the parent-side syscall sequence — write, then a read that only
 * returns after the child ran — is visible in the `s` panel; the switch
 * itself happens in the kernel and is invisible by design.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WARMUP        1000
#define SC_BASE_ITERS 200000

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double zero_read_cost_ns(void)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/null");
        exit(1);
    }
    char buf[1];
    volatile long sink = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < SC_BASE_ITERS; i++)
        sink += read(fd, buf, 0);
    uint64_t dt = now_ns() - t0;
    close(fd);
    (void)sink;
    return (double)dt / (double)SC_BASE_ITERS;
}

int main(int argc, char **argv)
{
    int  cpu   = (argc > 1) ? atoi(argv[1]) : 0;
    long trips = (argc > 2) ? atol(argv[2]) : 50000;
    long ncpu  = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 0 || cpu >= ncpu || trips < 1) {
        fprintf(stderr, "usage: %s [cpu 0..%ld] [round_trips >= 1]\n",
                argv[0], ncpu - 1);
        return 2;
    }

    /* pin BEFORE fork so the child inherits the same single-CPU mask —
     * without this, the two processes can run in parallel on two cores
     * and the measurement stops being about context switches */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) < 0) {
        perror("sched_setaffinity");
        return 1;
    }

    double sc_ns = zero_read_cost_ns();

    int p1[2], p2[2]; /* parent -> child, child -> parent */
    if (pipe(p1) < 0 || pipe(p2) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        /* child: echo one token back forever, until pipe1 closes */
        close(p1[1]);
        close(p2[0]);
        char b;
        while (read(p1[0], &b, 1) == 1) {
            if (write(p2[1], &b, 1) != 1)
                break;
        }
        _exit(0);
    }
    close(p1[0]);
    close(p2[1]);

    char b = 'x';
    for (int i = 0; i < WARMUP; i++) {
        if (write(p1[1], &b, 1) != 1 || read(p2[0], &b, 1) != 1) {
            perror("warmup ping-pong");
            return 1;
        }
    }

    uint64_t t0 = now_ns();
    for (long i = 0; i < trips; i++) {
        if (write(p1[1], &b, 1) != 1 || read(p2[0], &b, 1) != 1) {
            perror("ping-pong");
            return 1;
        }
    }
    uint64_t dt = now_ns() - t0;

    close(p1[1]); /* child's read returns 0 -> it exits */
    close(p2[0]);
    waitpid(pid, NULL, 0);

    double rt = (double)dt / (double)trips;
    double corrected = (rt - 4.0 * sc_ns) / 2.0;

    printf("cpu %d | %ld round trips | total %.3f ms\n",
           cpu, trips, (double)dt / 1e6);
    printf("  0-byte read baseline : %8.1f ns/syscall (x%d)\n",
           sc_ns, SC_BASE_ITERS);
    printf("  per round trip       : %8.1f ns\n", rt);
    printf("  raw estimate         : %8.1f ns/switch  (round trip / 2)\n",
           rt / 2.0);
    if (corrected > 0)
        printf("  corrected estimate   : %8.1f ns/switch  "
               "((round trip - 4*syscall) / 2)\n", corrected);
    else
        printf("  corrected estimate   : n/a (syscall baseline dominates; "
               "increase trips or measure natively)\n");
    printf("  -> true cost likely lies between the two estimates; "
           "run several times and take the median.\n");
    return 0;
}
