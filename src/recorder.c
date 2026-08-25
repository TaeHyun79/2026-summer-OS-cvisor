/* recorder.c - ptrace record engine with follow-fork/exec
 *
 * fork -> root child: TRACEME + ASLR off + stdout/stderr piped + exec
 *      -> parent: multi-tracee SINGLESTEP state machine.
 *
 * Every followed process gets its own step stream (proc_t); a global
 * sequence number (gseq) preserves the real interleaving order across
 * processes.  PTRACE_O_TRACEFORK/VFORK/CLONE auto-attaches children,
 * TRACEEXEC re-analyzes the new binary on execve (falling back to
 * "not followed" when it cannot be analyzed, e.g. PIE system binaries).
 *
 * The resume-all-then-waitpid(-1) pattern is what makes blocking syscalls
 * work: a parent sitting in wait4() simply produces no stop events while
 * the child keeps stepping.
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

/* in-flight syscall observed at its `syscall` instruction */
typedef struct {
    int      active;
    int64_t  nr;
    uint64_t args[6];
} pending_sc_t;

typedef struct {
    pid_t    pid;
    int      used, alive;
    int      fresh;        /* awaiting the first (attach) stop */
    int      pending_stop; /* stopped before we learned who it is; held
                            * suspended until register_child() links it */
    int      followed;     /* stepping+recording; 0 = free-running (CONT) */
    int      detached;     /* over proc limit: PTRACE_DETACHed */
    int      dying;        /* fatal signal delivered, waiting for the exit */
    int      recording;
    int      proc;         /* trace proc index, -1 if not registered */
    int      img;          /* current image index */
    uint64_t stack_top;
    range_t  brk_heap;
    range_t  anonr[CV_MAX_HEAPR - 1];
    int      n_anonr;
    pending_sc_t psc;
    uint32_t skipped;
    size_t   steps_seen;   /* for the periodic maps recheck */
    int      mem_fd;
    int      use_pvr;
    struct user_regs_struct regs;
} tracee_t;

typedef struct {
    trace_t  *t;
    tracee_t  tr[CV_MAX_PROCS * 2]; /* room for detach-tracked overflow */
    int       n_tr;
    int       from_main;
    size_t    max_steps;
    int       out_fd;
} rec_t;

/* ---------------- target memory access ---------------- */

static ssize_t read_mem(tracee_t *tr, uint64_t addr, void *buf, size_t len)
{
    if (tr->use_pvr) {
        struct iovec local  = { buf, len };
        struct iovec remote = { (void *)addr, len };
        ssize_t n = process_vm_readv(tr->pid, &local, 1, &remote, 1, 0);
        if (n >= 0)
            return n;
        if (errno == ENOSYS || errno == EPERM)
            tr->use_pvr = 0; /* fall through to /proc/<pid>/mem */
        else
            return -1;
    }
    if (tr->mem_fd < 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", tr->pid);
        tr->mem_fd = open(path, O_RDONLY);
        if (tr->mem_fd < 0)
            return -1;
    }
    return pread(tr->mem_fd, buf, len, (off_t)addr);
}

static void read_mem_zerofill(tracee_t *tr, uint64_t addr, uint8_t *buf,
                              size_t len)
{
    ssize_t n = read_mem(tr, addr, buf, len);
    if (n < 0)
        n = 0;
    if ((size_t)n < len)
        memset(buf + n, 0, len - (size_t)n);
}

/* ---------------- /proc/<pid>/maps ---------------- */

static int maps_find(pid_t pid, const char *tag, range_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, tag))
            continue;
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) == 2) {
            out->start = a;
            out->end = b;
            found = 0;
        }
        break;
    }
    fclose(fp);
    return found;
}

/* ---------------- output pipe ---------------- */

static void drain_output(rec_t *r, uint64_t gseq)
{
    char buf[4096];
    trace_t *t = r->t;
    for (;;) {
        ssize_t n = read(r->out_fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        size_t off = t->out_len;
        trace_append_output(t, buf, (size_t)n);
        outchunk_t *nc = cv_grow(t->chunks, t->n_chunks, &t->cap_chunks,
                                 sizeof(outchunk_t), 64);
        if (!nc)
            return;
        t->chunks = nc;
        t->chunks[t->n_chunks++] = (outchunk_t){ gseq, off, (size_t)n };
    }
}

/* ---------------- snapshot ---------------- */

static int snapshot(rec_t *r, tracee_t *tr)
{
    trace_t *t = r->t;
    proc_t  *p = &t->procs[tr->proc];
    const image_t *im = t->images[tr->img];

    step_t *s = proc_new_step(p);
    if (!s)
        return -1;
    s->regs = tr->regs;
    s->gseq = t->gseq_end++;
    s->img  = tr->img;
    s->skipped = tr->skipped;
    tr->skipped = 0;

    /* stack: [rsp - red zone, stack_top), capped */
    uint64_t lo = tr->regs.rsp - CV_STACK_RED_ZONE;
    lo &= ~(uint64_t)7;
    uint64_t hi = tr->stack_top;
    if (hi > lo && hi - lo > CV_STACK_SNAP_MAX)
        hi = lo + CV_STACK_SNAP_MAX;
    if (hi > lo) {
        size_t len = hi - lo;
        s->stack = malloc(len);
        if (s->stack) {
            read_mem_zerofill(tr, lo, s->stack, len);
            s->stack_len = len;
            s->stack_base = lo;
        }
    }

    /* heap: [heap] mapping + tracked anonymous mmaps, fair-split budget */
    size_t budget = CV_HEAP_SNAP_MAX;
    range_t regions[CV_MAX_HEAPR];
    int nregions = 0;
    if (tr->brk_heap.end > tr->brk_heap.start)
        regions[nregions++] = tr->brk_heap;
    for (int i = 0; i < tr->n_anonr && nregions < CV_MAX_HEAPR; i++)
        regions[nregions++] = tr->anonr[i];
    for (int i = 0; i < nregions && budget > 0; i++) {
        size_t share = budget / (size_t)(nregions - i);
        size_t len = regions[i].end - regions[i].start;
        if (len > share)
            len = share;
        uint8_t *buf = malloc(len);
        if (!buf)
            break;
        read_mem_zerofill(tr, regions[i].start, buf, len);
        heapreg_t *hr = &s->heapr[s->n_heapr++];
        hr->buf = buf;
        hr->base = regions[i].start;
        hr->len = len;
        budget -= len;
    }

    /* globals: .got + .data + .bss covering range of the current image */
    if (im->globals_rng.end > im->globals_rng.start) {
        size_t len = im->globals_rng.end - im->globals_rng.start;
        s->globals = malloc(len);
        if (s->globals) {
            read_mem_zerofill(tr, im->globals_rng.start, s->globals, len);
            s->globals_len = len;
        }
    }

    s->insn_idx = img_insn_lookup(im, tr->regs.rip);
    s->src_line = img_line_lookup(im, tr->regs.rip);
    return 0;
}

/* ---------------- syscall handling ---------------- */

static void handle_syscall(rec_t *r, tracee_t *tr, int64_t ret)
{
    trace_t *t = r->t;
    const pending_sc_t *p = &tr->psc;

    if (tr->recording && tr->proc >= 0) {
        scevent_t *ns = cv_grow(t->scs, t->n_scs, &t->cap_scs,
                                sizeof(scevent_t), 64);
        if (ns) {
            t->scs = ns;
            proc_t *pp = &t->procs[tr->proc];
            scevent_t *e = &t->scs[t->n_scs++];
            e->proc = tr->proc;
            e->step = pp->n_steps ? pp->n_steps - 1 : 0;
            e->nr = p->nr;
            e->ret = ret;
            memcpy(e->args, p->args, sizeof(e->args));
        }
    }

    switch (p->nr) {
    case SYS_brk: /* [heap] may have just appeared or grown */
        maps_find(tr->pid, "[heap]", &tr->brk_heap);
        break;
    case SYS_mmap: { /* track anonymous writable mappings (big mallocs);
                      * only while recording, so loader-internal mmaps
                      * stay out */
        uint64_t uret = (uint64_t)ret;
        int anon     = (p->args[3] & MAP_ANONYMOUS) != 0;
        int writable = (p->args[2] & PROT_WRITE)    != 0;
        if (tr->recording && uret < 0xfffffffffffff000ULL && anon &&
            writable && p->args[1] > 0 && p->args[1] <= CV_MMAP_TRACK_MAX &&
            tr->n_anonr < CV_MAX_HEAPR - 1) {
            uint64_t len = (p->args[1] + 4095) & ~4095ULL;
            tr->anonr[tr->n_anonr++] = (range_t){ uret, uret + len };
        }
        break;
    }
    case SYS_munmap: { /* forget tracked regions inside the range */
        uint64_t lo = p->args[0], hi = p->args[0] + p->args[1];
        for (int i = 0; i < tr->n_anonr; ) {
            if (tr->anonr[i].start >= lo && tr->anonr[i].end <= hi)
                tr->anonr[i] = tr->anonr[--tr->n_anonr];
            else
                i++;
        }
        break;
    }
    }
}

/* ---------------- tracee bookkeeping ---------------- */

static tracee_t *find_tracee(rec_t *r, pid_t pid)
{
    for (int i = 0; i < r->n_tr; i++)
        if (r->tr[i].used && r->tr[i].pid == pid)
            return &r->tr[i];
    return NULL;
}

static tracee_t *add_tracee(rec_t *r, pid_t pid)
{
    if (r->n_tr >= (int)(sizeof(r->tr) / sizeof(r->tr[0])))
        return NULL;
    tracee_t *tr = &r->tr[r->n_tr++];
    memset(tr, 0, sizeof(*tr));
    tr->pid = pid;
    tr->used = 1;
    tr->alive = 1;
    tr->followed = 1;
    tr->proc = -1;
    tr->mem_fd = -1;
    tr->use_pvr = 1;
    return tr;
}

static void close_tracee(tracee_t *tr)
{
    if (tr->mem_fd >= 0) {
        close(tr->mem_fd);
        tr->mem_fd = -1;
    }
    tr->alive = 0;
}

static int process_and_resume(rec_t *r, tracee_t *tr);

/* register a newly forked child, inheriting the parent's context (COW
 * address space: same stack top, same heap regions, same image) */
static int register_child(rec_t *r, tracee_t *parent, pid_t cpid)
{
    trace_t *t = r->t;
    tracee_t *ct = find_tracee(r, cpid); /* SIGSTOP may have come first */
    if (!ct) {
        ct = add_tracee(r, cpid);
        if (!ct)
            return 0;
        ct->fresh = 1; /* its attach stop has not arrived yet */
    }

    if (t->n_procs >= CV_MAX_PROCS) {
        fprintf(stderr, "\ncvisor: process limit (%d) reached; pid %d "
                        "will run untraced\n", CV_MAX_PROCS, cpid);
        ct->followed = 0;
        ct->detached = 1; /* detach at its (next) stop */
        if (ct->pending_stop) {
            ct->pending_stop = 0;
            ptrace(PTRACE_DETACH, cpid, 0, 0);
        }
        return 0;
    }

    proc_t *cp = &t->procs[t->n_procs];
    memset(cp, 0, sizeof(*cp));
    cp->pid = cpid;
    cp->parent = parent->proc;
    cp->followed = 1;
    ct->proc = t->n_procs++;
    ct->followed = 1;

    ct->img       = parent->img;
    ct->recording = parent->recording;
    ct->stack_top = parent->stack_top;
    ct->brk_heap  = parent->brk_heap;
    memcpy(ct->anonr, parent->anonr, sizeof(ct->anonr));
    ct->n_anonr   = parent->n_anonr;

    if (ct->pending_stop) { /* it is already stopped and waiting for us */
        ct->pending_stop = 0;
        ct->fresh = 0;
        if (ptrace(PTRACE_GETREGS, cpid, 0, &ct->regs) == 0 &&
            process_and_resume(r, ct) < 0)
            return -1;
    }
    return 0;
}

/* ---------------- per-stop processing ---------------- */

/* returns -1 when the global step cap is hit */
static int process_and_resume(rec_t *r, tracee_t *tr)
{
    trace_t *t = r->t;
    const image_t *im = t->images[tr->img];

    if (tr->psc.active) {
        handle_syscall(r, tr, (int64_t)tr->regs.rax);
        tr->psc.active = 0;
    }

    if (tr->steps_seen++ % CV_HEAP_RECHECK == 0)
        maps_find(tr->pid, "[heap]", &tr->brk_heap);

    if (!tr->recording && tr->regs.rip == im->main_addr)
        tr->recording = 1;

    if (tr->recording && tr->proc >= 0 &&
        tr->regs.rip >= im->text.start && tr->regs.rip < im->text.end) {
        if (t->gseq_end >= r->max_steps) {
            t->truncated = 1;
            return -1;
        }
        if (snapshot(r, tr) < 0) {
            t->truncated = 1;
            return -1;
        }
        drain_output(r, t->gseq_end - 1);
        if (t->gseq_end % 500 == 0)
            fprintf(stderr, "\rRecording... %llu steps",
                    (unsigned long long)t->gseq_end);
    } else if (tr->recording) {
        tr->skipped++;
    }

    /* is the instruction about to execute a `syscall` (0f 05)?  Inside
     * the image's .text the bytes are already loaded (read-only mapping,
     * identical at runtime), which saves one process_vm_readv per step;
     * read_mem remains the fallback for libc addresses */
    uint8_t op[2];
    int have_op;
    uint64_t rip = tr->regs.rip;
    if (im->text_bytes && rip >= im->text.start && rip + 2 <= im->text.end) {
        memcpy(op, im->text_bytes + (rip - im->text.start), 2);
        have_op = 1;
    } else {
        have_op = (read_mem(tr, rip, op, 2) == 2);
    }
    if (have_op && op[0] == 0x0f && op[1] == 0x05) {
        tr->psc.active = 1;
        tr->psc.nr = (int64_t)tr->regs.rax;
        tr->psc.args[0] = tr->regs.rdi; tr->psc.args[1] = tr->regs.rsi;
        tr->psc.args[2] = tr->regs.rdx; tr->psc.args[3] = tr->regs.r10;
        tr->psc.args[4] = tr->regs.r8;  tr->psc.args[5] = tr->regs.r9;
    }

    ptrace(PTRACE_SINGLESTEP, tr->pid, 0, 0);
    return 0;
}

/* ---------------- exec handling ---------------- */

static void handle_exec(rec_t *r, tracee_t *tr)
{
    trace_t *t = r->t;
    proc_t *p = tr->proc >= 0 ? &t->procs[tr->proc] : NULL;

    if (tr->psc.active) { /* the execve "returned" into the new program */
        handle_syscall(r, tr, 0);
        tr->psc.active = 0;
    }
    if (p)
        p->execed = 1;

    char lnk[64], exe[512];
    snprintf(lnk, sizeof(lnk), "/proc/%d/exe", tr->pid);
    ssize_t n = readlink(lnk, exe, sizeof(exe) - 1);
    if (n <= 0)
        n = 0;
    exe[n] = '\0';

    /* reuse an already-analyzed image of the same binary */
    int img = -1;
    for (int i = 0; i < t->n_images; i++) {
        if (strcmp(t->images[i]->path, exe) == 0) {
            img = i;
            break;
        }
    }
    if (img < 0 && t->n_images < CV_MAX_IMAGES && exe[0]) {
        image_t *im = image_analyze(exe, 1 /* quiet */);
        if (im) {
            img = t->n_images;
            t->images[t->n_images++] = im;
        }
    }

    if (img < 0) {
        /* PIE / no DWARF / limit reached: stop following past the exec */
        fprintf(stderr, "\ncvisor: pid %d exec'd '%s' which cannot be "
                        "analyzed; not followed past the exec\n",
                tr->pid, exe[0] ? exe : "?");
        if (p)
            p->followed = 0;
        tr->followed = 0;
        ptrace(PTRACE_CONT, tr->pid, 0, 0);
        return;
    }

    /* fresh address space: reset the memory context for the new image */
    tr->followed = 1;
    if (p)
        p->followed = 1;
    tr->img = img;
    tr->brk_heap = (range_t){0, 0};
    tr->n_anonr = 0;
    tr->skipped = 0;
    tr->steps_seen = 0;
    if (tr->mem_fd >= 0) {
        close(tr->mem_fd);
        tr->mem_fd = -1;
    }
    tr->stack_top = tr->regs.rsp + CV_STACK_ABOVE_RSP0;
    tr->recording = !r->from_main;
    /* on a cap hit process_and_resume set t->truncated; the record()
     * loop checks it right after this call */
    process_and_resume(r, tr);
}

/* ---------------- main record loop ---------------- */

static void kill_all(rec_t *r)
{
    for (int i = 0; i < r->n_tr; i++) {
        if (r->tr[i].used && r->tr[i].alive && !r->tr[i].detached) {
            kill(r->tr[i].pid, SIGKILL);
            waitpid(r->tr[i].pid, NULL, 0);
            close_tracee(&r->tr[i]);
        }
    }
}

static int any_alive(rec_t *r)
{
    for (int i = 0; i < r->n_tr; i++)
        if (r->tr[i].used && r->tr[i].alive && !r->tr[i].detached)
            return 1;
    return 0;
}

int record(trace_t *t, const char *target_path, char *const argv[],
           int from_main, size_t max_steps)
{
    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("cvisor: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("cvisor: fork");
        return -1;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        personality(ADDR_NO_RANDOMIZE); /* fixed stack/heap addresses */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execv(target_path, argv);
        _exit(127);
    }

    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    static rec_t r; /* large (tracee array); keep off the stack */
    memset(&r, 0, sizeof(r));
    r.t = t;
    r.from_main = from_main;
    r.max_steps = max_steps;
    r.out_fd = pfd[0];

    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "cvisor: target failed to start\n");
        close(pfd[0]);
        return -1;
    }
    ptrace(PTRACE_SETOPTIONS, pid, 0,
           PTRACE_O_EXITKILL | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
           PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC);

    tracee_t *root = add_tracee(&r, pid);
    root->fresh = 0;
    root->img = 0;
    root->recording = !from_main;
    root->proc = 0;
    t->n_procs = 1;
    memset(&t->procs[0], 0, sizeof(t->procs[0]));
    t->procs[0].pid = pid;
    t->procs[0].parent = -1;
    t->procs[0].followed = 1;

    if (from_main && t->images[0]->main_addr == 0) {
        fprintf(stderr, "cvisor: --from-main: 'main' symbol not found; "
                        "recording from _start\n");
        root->recording = 1;
    }

    if (ptrace(PTRACE_GETREGS, pid, 0, &root->regs) < 0) {
        perror("cvisor: PTRACE_GETREGS");
        goto fail;
    }
    root->stack_top = root->regs.rsp + CV_STACK_ABOVE_RSP0;

    fprintf(stderr, "Recording... ");
    if (process_and_resume(&r, root) < 0)
        goto done_truncated;

    while (any_alive(&r)) {
        pid_t wpid = waitpid(-1, &status, __WALL);
        if (wpid < 0) {
            if (errno == ECHILD)
                break;
            if (errno == EINTR)
                continue;
            perror("\ncvisor: waitpid");
            break;
        }

        tracee_t *tr = find_tracee(&r, wpid);
        if (!tr) {
            /* a forked child stopped before its parent's fork event told
             * us about it: hold it suspended; register_child resumes it */
            if (WIFSTOPPED(status)) {
                tr = add_tracee(&r, wpid);
                if (tr) {
                    tr->fresh = 1;
                    tr->pending_stop = 1;
                    tr->followed = 0; /* until register_child links it */
                }
            }
            continue;
        }

        if (WIFEXITED(status)) {
            if (tr->proc >= 0)
                t->procs[tr->proc].exit_code = WEXITSTATUS(status);
            close_tracee(tr);
            continue;
        }
        if (WIFSIGNALED(status)) {
            if (tr->proc >= 0)
                t->procs[tr->proc].death_signal = WTERMSIG(status);
            close_tracee(tr);
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        unsigned event = (unsigned)status >> 16;

        if (tr->detached) { /* over the proc limit: let it run free */
            ptrace(PTRACE_DETACH, wpid, 0, sig == SIGSTOP ? 0 : sig);
            continue;
        }
        if (tr->dying) { /* fatal signal already snapshotted */
            ptrace(PTRACE_CONT, wpid, 0, sig);
            continue;
        }
        if (!tr->followed && event != PTRACE_EVENT_EXEC) {
            /* free-running (exec'd into unanalyzable binary): pass signals
             * through, keep fork options doing nothing special */
            ptrace(PTRACE_CONT, wpid, 0, sig == SIGTRAP ? 0 : sig);
            continue;
        }

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
            event == PTRACE_EVENT_CLONE) {
            unsigned long cpid = 0;
            ptrace(PTRACE_GETEVENTMSG, wpid, 0, &cpid);
            if (ptrace(PTRACE_GETREGS, wpid, 0, &tr->regs) == 0) {
                /* at the event stop RAX still holds the in-kernel
                 * placeholder (-ENOSYS); the real return value in the
                 * parent is the child's pid from the event message */
                if (tr->psc.active) {
                    handle_syscall(&r, tr, (int64_t)cpid);
                    tr->psc.active = 0;
                }
                if (register_child(&r, tr, (pid_t)cpid) < 0)
                    goto done_truncated;
                if (process_and_resume(&r, tr) < 0)
                    goto done_truncated;
            }
            continue;
        }

        if (event == PTRACE_EVENT_EXEC) {
            if (ptrace(PTRACE_GETREGS, wpid, 0, &tr->regs) == 0) {
                handle_exec(&r, tr);
                if (t->truncated)
                    goto done_truncated;
            }
            continue;
        }

        if (sig == SIGTRAP || (tr->fresh && sig == SIGSTOP)) {
            tr->fresh = 0;
            if (ptrace(PTRACE_GETREGS, wpid, 0, &tr->regs) < 0) {
                /* died under us; the exit event will follow */
                ptrace(PTRACE_CONT, wpid, 0, 0);
                continue;
            }
            if (process_and_resume(&r, tr) < 0)
                goto done_truncated;
            continue;
        }

        if (sig == SIGCHLD || sig == SIGWINCH || sig == SIGURG ||
            sig == SIGCONT || sig == SIGSTOP) {
            /* harmless: the pending insn did not run; deliver and retry
             * (rip unchanged, so no re-snapshot, no double count) */
            tr->psc.active = 0;
            ptrace(PTRACE_SINGLESTEP, wpid, 0, sig == SIGSTOP ? 0 : sig);
            continue;
        }

        /* fatal (e.g. SIGSEGV): snapshot the dying state, then let the
         * process take the signal for real */
        tr->psc.active = 0;
        if (tr->proc >= 0) {
            t->procs[tr->proc].death_signal = sig;
            if (ptrace(PTRACE_GETREGS, wpid, 0, &tr->regs) == 0 &&
                t->gseq_end < r.max_steps && tr->recording)
                snapshot(&r, tr);
        }
        tr->dying = 1;
        ptrace(PTRACE_CONT, wpid, 0, sig);
    }

    drain_output(&r, t->gseq_end ? t->gseq_end - 1 : 0);
    fprintf(stderr, "\rRecording... %llu steps, %d process%s (done)\n",
            (unsigned long long)t->gseq_end, t->n_procs,
            t->n_procs == 1 ? "" : "es");
    close(pfd[0]);
    if (t->gseq_end == 0) {
        if (t->procs[0].exit_code == 127)
            fprintf(stderr, "cvisor: exec failed — is '%s' executable?\n",
                    target_path);
        else
            fprintf(stderr, "cvisor: no steps recorded (nothing to show)\n");
        return -1;
    }
    return 0;

done_truncated:
    kill_all(&r);
    drain_output(&r, t->gseq_end ? t->gseq_end - 1 : 0);
    fprintf(stderr, "\rRecording... %llu steps (cap reached, truncated)\n",
            (unsigned long long)t->gseq_end);
    close(pfd[0]);
    return t->gseq_end > 0 ? 0 : -1;

fail:
    kill_all(&r);
    close(pfd[0]);
    return -1;
}

/* ---------------- --trace text dump (verification) ---------------- */

void record_dump(const trace_t *t)
{
    for (int pi = 0; pi < t->n_procs; pi++) {
        const proc_t *p = &t->procs[pi];
        const char *exsuf =
            !p->execed  ? ""
            : p->followed ? " | exec'd (followed)"
                          : " | exec'd (NOT followed)";
        if (p->parent < 0)
            printf("=== proc %d | pid %d | root%s ===\n", pi, p->pid, exsuf);
        else
            printf("=== proc %d | pid %d | child of proc %d (pid %d)%s ===\n",
                   pi, p->pid, p->parent, t->procs[p->parent].pid, exsuf);

        size_t sc = 0;
        for (size_t i = 0; i < p->n_steps; i++) {
            const step_t *s = &p->steps[i];
            const image_t *im = t->images[s->img];
            const char *insn = (s->insn_idx >= 0)
                                   ? im->dlines[s->insn_idx].text : "?";
            printf("g%llu step %zu | rip=0x%llx | %s:%d | %s",
                   (unsigned long long)s->gseq, i,
                   (unsigned long long)s->regs.rip,
                   im->src_file[0] ? im->src_file : "?", s->src_line, insn);
            if (s->skipped)
                printf(" | +%u libc insns", s->skipped);
            if (i > 0) {
                const step_t *pv = &p->steps[i - 1];
                for (int rn = 0; rn < CV_NREGS; rn++) {
                    uint64_t ov = cv_reg(&pv->regs, rn);
                    uint64_t nv = cv_reg(&s->regs, rn);
                    if (ov != nv && rn != CV_REG_RIP)
                        printf(" | %s %llx->%llx", CV_REGS[rn].name,
                               (unsigned long long)ov,
                               (unsigned long long)nv);
                }
            }
            printf("\n");

            for (; sc < t->n_scs; sc++) {
                if (t->scs[sc].proc != pi)
                    continue;
                if (t->scs[sc].step > i)
                    break;
                char scbuf[128];
                cv_format_syscall(&t->scs[sc], scbuf, sizeof(scbuf));
                printf("    syscall %s\n", scbuf);
            }
        }

        printf("=== proc %d: %zu steps | ", pi, p->n_steps);
        if (p->death_signal)
            printf("killed by signal %d (%s)", p->death_signal,
                   strsignal(p->death_signal));
        else
            printf("exit code %d", p->exit_code);
        printf(" ===\n");
    }

    printf("== total %llu steps, %d procs%s ==\n",
           (unsigned long long)t->gseq_end, t->n_procs,
           t->truncated ? " | TRUNCATED at cap" : "");
    if (t->out_len) {
        printf("== program output (%zu bytes) ==\n", t->out_len);
        fwrite(t->prog_output, 1, t->out_len, stdout);
        if (t->prog_output[t->out_len - 1] != '\n')
            printf("\n");
    }
}
