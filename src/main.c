/* main.c - cvisor entry point
 *
 * usage: cvisor [options] <target> [target args...]
 *   --dump         static analysis only, print parsed results (Phase 0)
 *   --trace        record, then dump the trace as text (Phase 1)
 *   --from-main    skip recording until RIP reaches main
 *   --max-steps N  recording cap (default 200000)
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--dump] [--trace] [--from-main] [--max-steps N] "
        "<target> [args...]\n"
        "\n"
        "target must be built with:\n"
        "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n",
        argv0);
}

int main(int argc, char **argv)
{
    int opt_dump = 0, opt_trace = 0, opt_from_main = 0;
    size_t max_steps = CV_DEFAULT_MAX_STEPS;

    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            opt_dump = 1;
        else if (strcmp(argv[i], "--trace") == 0)
            opt_trace = 1;
        else if (strcmp(argv[i], "--from-main") == 0)
            opt_from_main = 1;
        else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc)
            max_steps = (size_t)strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "cvisor: unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else
            break;
    }
    if (i >= argc) {
        usage(argv[0]);
        return 2;
    }
    const char *target = argv[i];
    char *const *target_argv = &argv[i]; /* argv[0] = target path */

    trace_t t;
    memset(&t, 0, sizeof(t));

    /* [A] static analysis */
    if (analyze(&t, target) < 0)
        return 1;
    if (opt_dump) {
        analyze_dump(&t);
        trace_free(&t);
        return 0;
    }

    /* [B] record */
    if (record(&t, target, target_argv, opt_from_main, max_steps) < 0) {
        trace_free(&t);
        return 1;
    }
    if (opt_trace) {
        record_dump(&t);
        trace_free(&t);
        return 0;
    }

    /* [C] replay TUI */
    int rc = tui_run(&t);

    /* post-TUI summary on the normal terminal */
    printf("cvisor: %zu steps recorded", t.n_steps);
    if (t.truncated)
        printf(" (truncated at cap)");
    if (t.death_signal)
        printf("; target killed by signal %d", t.death_signal);
    else
        printf("; target exited with code %d", t.exit_code);
    printf("\n");

    trace_free(&t);
    return rc == 0 ? 0 : 1;
}
