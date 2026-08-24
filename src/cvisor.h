/*
 * cvisor - C program execution visualizer (record & replay tracer)
 *
 * Target environment: Linux x86-64 (OSTEP-compatible userland).
 * See README.md for the development environment setup on non-Linux hosts.
 */
#ifndef CVISOR_H
#define CVISOR_H

#if !defined(__linux__) || !defined(__x86_64__)
#error "cvisor targets Linux x86-64 only (see README.md, section 2)"
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/user.h> /* struct user_regs_struct */

/* ---------- limits / tunables ---------- */

#define CV_DEFAULT_MAX_STEPS 200000  /* recording cap (spec 6.2.3) */
#define CV_STACK_RED_ZONE    64      /* bytes below RSP to snapshot */
#define CV_STACK_ABOVE_RSP0  256     /* stack top = initial RSP + this */
#define CV_STACK_SNAP_MAX    8192    /* per-step stack snapshot cap */
#define CV_HEAP_SNAP_MAX     65536   /* per-step heap snapshot cap (total) */
#define CV_GLOBALS_MAX       65536   /* .data + .bss covering range cap */
#define CV_HEAP_RECHECK      128     /* re-read /proc/pid/maps every N steps */
#define CV_MAX_HEAPR         8       /* tracked heap regions: [heap] + mmaps */
#define CV_MMAP_TRACK_MAX    (16u << 20) /* ignore anon mmaps larger than this */

/* ---------- static analysis ---------- */

typedef struct {
    uint64_t start, end; /* [start, end) */
} range_t;

/* one display line of the disassembly panel (instruction or "<sym>:" label) */
typedef struct {
    uint64_t addr;
    int      is_label;
    char    *text;
} dline_t;

/* sorted index over instruction lines only: addr -> dlines index */
typedef struct {
    uint64_t addr;
    int32_t  dline_idx;
} insn_ref_t;

/* addr -> source line (sorted by addr; line == -1 marks end-of-sequence) */
typedef struct {
    uint64_t addr;
    int32_t  line;
} lmap_t;

/* ---------- recording ---------- */

/* one snapshotted heap region: the brk [heap] or an anonymous rw mmap */
typedef struct {
    uint8_t *buf;
    uint64_t base;
    size_t   len;
} heapreg_t;

typedef struct {
    struct user_regs_struct regs;
    uint8_t  *stack;   size_t stack_len; uint64_t stack_base;
    heapreg_t heapr[CV_MAX_HEAPR]; int n_heapr;
    uint8_t  *globals; size_t globals_len; /* base is trace-wide */
    int32_t   src_line;  /* 1-based, -1 = no mapping */
    int32_t   insn_idx;  /* index into dlines, -1 = unknown */
    uint32_t  skipped;   /* unrecorded (libc) instructions executed since
                          * the previous recorded step */
} step_t;

/* a syscall observed while stepping (recorded steps only) */
typedef struct {
    size_t   step;    /* last recorded step index before the syscall */
    int64_t  nr;
    uint64_t args[6]; /* rdi rsi rdx r10 r8 r9 at the syscall insn */
    int64_t  ret;
} scevent_t;

/* a chunk of target stdout/stderr, attributed to the step it appeared at */
typedef struct {
    size_t step;
    size_t off, len; /* into trace_t.prog_output */
} outchunk_t;

typedef struct {
    /* [A] static analysis */
    dline_t    *dlines;  size_t n_dlines;
    insn_ref_t *irefs;   size_t n_irefs;
    lmap_t     *lmap;    size_t n_lmap;
    char      **src;     int    n_src;
    char        src_file[512];
    range_t     text;
    range_t     globals_rng;   /* covering range of .data + .bss */
    uint64_t    main_addr;     /* 0 if not found */
    uint64_t    entry;

    /* [B] recording */
    step_t     *steps;   size_t n_steps, cap_steps;
    int         truncated;
    int         exit_code;     /* valid when death_signal == 0 */
    int         death_signal;  /* 0 = normal exit */
    char       *prog_output; size_t out_len, out_cap;
    outchunk_t *chunks;      size_t n_chunks, cap_chunks;
    scevent_t  *scs;         size_t n_scs, cap_scs;
    int64_t     fork_step;     /* first step where fork/clone/exec was seen,
                                * -1 = never (children are NOT followed) */
} trace_t;

const char *cv_syscall_name(int64_t nr); /* trace.c; NULL if unknown */

/* ---------- register table (recorder dump + TUI share this) ---------- */

typedef struct {
    const char *name;
    size_t      off; /* offsetof into struct user_regs_struct */
} regdesc_t;

extern const regdesc_t CV_REGS[];
extern const int       CV_NREGS;

uint64_t cv_reg(const struct user_regs_struct *r, int i);

/* ---------- analyzer.c ---------- */

int  analyze(trace_t *t, const char *target_path);
void analyze_dump(const trace_t *t);

/* ---------- recorder.c ---------- */

int  record(trace_t *t, const char *target_path, char *const argv[],
            int from_main, size_t max_steps);
void record_dump(const trace_t *t);

/* ---------- trace.c ---------- */

int32_t trace_insn_lookup(const trace_t *t, uint64_t rip); /* -> dlines idx */
int32_t trace_line_lookup(const trace_t *t, uint64_t rip); /* -> src line  */
step_t *trace_new_step(trace_t *t);                        /* NULL if full */
void    trace_append_output(trace_t *t, const char *buf, size_t len);
void    trace_free(trace_t *t);

/* ---------- tui.c ---------- */

int tui_run(trace_t *t);

#endif /* CVISOR_H */
