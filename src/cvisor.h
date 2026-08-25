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
#include <sys/types.h>
#include <sys/user.h> /* struct user_regs_struct */

/* ---------- limits / tunables ---------- */

#define CV_DEFAULT_MAX_STEPS 200000  /* recording cap, total across procs */
#define CV_STACK_RED_ZONE    64      /* bytes below RSP to snapshot */
#define CV_STACK_ABOVE_RSP0  256     /* stack top = initial RSP + this */
#define CV_STACK_SNAP_MAX    8192    /* per-step stack snapshot cap */
#define CV_HEAP_SNAP_MAX     65536   /* per-step heap snapshot cap (total) */
#define CV_GLOBALS_MAX       65536   /* .got + .data + .bss covering cap */
#define CV_HEAP_RECHECK      128     /* re-read /proc/pid/maps every N steps */
#define CV_MAX_HEAPR         8       /* tracked heap regions: [heap] + mmaps */
#define CV_STATIC_SEC_MAX    (1u << 20) /* .text/.rodata load cap (bytes) */
#define CV_MMAP_TRACK_MAX    (16u << 20) /* ignore anon mmaps larger than this */
#define CV_MAX_PROCS         8       /* followed processes (fork tree) */
#define CV_MAX_IMAGES        8       /* analyzed binaries (root + execs) */

/* ---------- static analysis: one ELF binary = one image ---------- */

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

/* ---------- DWARF variable info (libdw, spec 6.4) ---------- */

/* one variable: a local/param (frame-relative) or a global (absolute).
 * At -O0 every location is either DW_OP_fbreg <off> or DW_OP_addr. */
typedef struct {
    char     name[64];
    char     type[64];
    int      is_param;
    int      is_global;  /* 1: `addr` valid; 0: `off` is frame-relative */
    int64_t  off;        /* offset from the frame base */
    uint64_t addr;
    uint32_t size;       /* byte size, 0 = unknown/aggregate */
    int      enc;        /* DW_ATE_* encoding, 0 = unknown */
    int      is_ptr;
} dvar_t;

/* one function's scope: [lo, hi) plus how to compute its frame base.
 * gcc -O0 emits DW_AT_frame_base = DW_OP_call_frame_cfa, which with a
 * frame pointer equals RBP + 16 inside the body; older styles use
 * DW_OP_breg6 (RBP-relative) directly. */
typedef struct {
    uint64_t lo, hi;
    char     name[64];
    int      fb_cfa;     /* 1: frame base = RBP + 16 (CFA) */
    int64_t  fb_off;     /* when !fb_cfa: frame base = RBP + fb_off */
    dvar_t  *vars; int n_vars;
} dfunc_t;

typedef struct {
    char        path[512];
    dline_t    *dlines;  size_t n_dlines;
    insn_ref_t *irefs;   size_t n_irefs;
    lmap_t     *lmap;    size_t n_lmap;
    char      **src;     int    n_src;
    char        src_file[512];
    range_t     text;
    range_t     globals_rng;   /* covering range of .got + .data + .bss */
    range_t     rodata_rng;    /* .rodata, 0/0 if absent */
    uint8_t    *text_bytes;    /* read-only sections, loaded once from the
                                * ELF file — identical at runtime (-no-pie,
                                * mapped r-x / r--) */
    uint8_t    *rodata_bytes;
    uint64_t    main_addr;     /* 0 if not found */
    uint64_t    entry;
    dfunc_t    *funcs; int n_funcs; /* sorted by lo; empty if no libdw info */
    dvar_t     *gvars; int n_gvars; /* global variables (DW_OP_addr) */
} image_t;

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
    uint8_t  *globals; size_t globals_len; /* base: image globals_rng */
    uint64_t  gseq;      /* global order across all processes */
    int32_t   img;       /* index into trace images (changes on exec) */
    int32_t   src_line;  /* 1-based, -1 = no mapping */
    int32_t   insn_idx;  /* index into image dlines, -1 = unknown */
    uint32_t  skipped;   /* unrecorded (libc) instructions executed since
                          * the previous recorded step of this process */
} step_t;

/* one followed process (fork tree member) */
typedef struct {
    pid_t    pid;
    int      parent;        /* proc index, -1 = the root process */
    step_t  *steps;  size_t n_steps, cap_steps;
    int      exit_code;     /* valid when death_signal == 0 */
    int      death_signal;  /* 0 = normal exit */
    int      execed;        /* an execve was seen */
    int      followed;      /* 0 = exec'd into a binary we cannot analyze
                             * (e.g. PIE): trace ends at the exec */
} proc_t;

/* a syscall observed while stepping (recorded steps only) */
typedef struct {
    int      proc;    /* proc index */
    size_t   step;    /* last recorded step of that proc before the call */
    int64_t  nr;
    uint64_t args[6]; /* rdi rsi rdx r10 r8 r9 at the syscall insn */
    int64_t  ret;
} scevent_t;

/* a chunk of target stdout/stderr (all procs share the inherited pipe) */
typedef struct {
    uint64_t gseq;
    size_t   off, len; /* into trace_t.prog_output */
} outchunk_t;

typedef struct {
    image_t    *images[CV_MAX_IMAGES]; int n_images;
    proc_t      procs[CV_MAX_PROCS];   int n_procs;
    int         truncated;
    uint64_t    gseq_end;    /* total recorded steps across procs */
    char       *prog_output; size_t out_len, out_cap;
    outchunk_t *chunks;      size_t n_chunks, cap_chunks;
    scevent_t  *scs;         size_t n_scs, cap_scs;
} trace_t;

/* ---------- register table (recorder dump + TUI share this) ---------- */

typedef struct {
    const char *name;
    size_t      off; /* offsetof into struct user_regs_struct */
} regdesc_t;

extern const regdesc_t CV_REGS[];
extern const int       CV_NREGS;

uint64_t    cv_reg(const struct user_regs_struct *r, int i);
const char *cv_syscall_name(int64_t nr); /* NULL if unknown */

/* ---------- analyzer.c ---------- */

image_t *image_analyze(const char *target_path, int quiet);
void     image_dump(const image_t *img);
void     image_free(image_t *img);

/* ---------- dwarfvars.c (libdw) ---------- */

int            dw_load_vars(image_t *img, const char *path); /* soft-fail */
const dfunc_t *img_func_at(const image_t *img, uint64_t rip);
/* frame-relative var -> absolute address given the function's frame base */
uint64_t       dvar_addr(const dfunc_t *f, const dvar_t *v, uint64_t rbp);

/* ---------- recorder.c ---------- */

int  record(trace_t *t, const char *target_path, char *const argv[],
            int from_main, size_t max_steps);
void record_dump(const trace_t *t);

/* ---------- trace.c ---------- */

int32_t img_insn_lookup(const image_t *img, uint64_t rip);
int32_t img_line_lookup(const image_t *img, uint64_t rip);
step_t *proc_new_step(proc_t *p);            /* NULL on OOM */
void    trace_append_output(trace_t *t, const char *buf, size_t len);
void    trace_free(trace_t *t);

/* ---------- tui.c ---------- */

int tui_run(trace_t *t);

#endif /* CVISOR_H */
