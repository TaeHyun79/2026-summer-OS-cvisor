/* recorder.c - ptrace record engine (spec 6.2)
 *
 * fork -> child: TRACEME + ASLR off + stdout/stderr piped + exec
 *      -> parent: SINGLESTEP loop; per step snapshot of registers +
 *         stack/heap/globals into trace[].  Steps whose RIP is outside the
 *         target .text (libc internals) advance without being recorded.
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    pid_t    pid;
    int      mem_fd;        /* /proc/<pid>/mem fallback, -1 if unopened */
    int      use_pvr;       /* process_vm_readv works */
    uint64_t stack_top;     /* initial RSP + CV_STACK_ABOVE_RSP0 */
    range_t  brk_heap;      /* current [heap] mapping, 0/0 if none */
    range_t  anonr[CV_MAX_HEAPR - 1]; /* anonymous rw mmaps (from syscalls) */
    int      n_anonr;
    int      out_fd;        /* read end of the child's stdout/stderr pipe */
} rec_ctx_t;

/* in-flight syscall observed at its `syscall` instruction */
typedef struct {
    int      active;
    int64_t  nr;
    uint64_t args[6];
} pending_sc_t;

/* ---------------- target memory access ---------------- */

static ssize_t read_mem(rec_ctx_t *c, uint64_t addr, void *buf, size_t len)
{
    if (c->use_pvr) {
        struct iovec local  = { buf, len };
        struct iovec remote = { (void *)addr, len };
        ssize_t n = process_vm_readv(c->pid, &local, 1, &remote, 1, 0);
        if (n >= 0)
            return n;
        if (errno == ENOSYS || errno == EPERM)
            c->use_pvr = 0; /* fall through to /proc/<pid>/mem */
        else
            return -1;
    }
    if (c->mem_fd < 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", c->pid);
        c->mem_fd = open(path, O_RDONLY);
        if (c->mem_fd < 0)
            return -1;
    }
    return pread(c->mem_fd, buf, len, (off_t)addr);
}

/* read [addr, addr+len); zero-fill whatever cannot be read */
static void read_mem_zerofill(rec_ctx_t *c, uint64_t addr, uint8_t *buf,
                              size_t len)
{
    ssize_t n = read_mem(c, addr, buf, len);
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

static void drain_output(rec_ctx_t *c, trace_t *t, size_t step_idx)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(c->out_fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        size_t off = t->out_len;
        trace_append_output(t, buf, (size_t)n);
        if (t->n_chunks == t->cap_chunks) {
            size_t ncap = t->cap_chunks ? t->cap_chunks * 2 : 64;
            outchunk_t *nc = realloc(t->chunks, ncap * sizeof(outchunk_t));
            if (!nc)
                return;
            t->chunks = nc;
            t->cap_chunks = ncap;
        }
        t->chunks[t->n_chunks++] = (outchunk_t){ step_idx, off, (size_t)n };
    }
}

/* ---------------- snapshot ---------------- */

static int snapshot(rec_ctx_t *c, trace_t *t,
                    const struct user_regs_struct *regs)
{
    step_t *s = trace_new_step(t);
    if (!s)
        return -1;
    s->regs = *regs;

    /* stack: [rsp - red zone, stack_top), capped */
    uint64_t lo = regs->rsp - CV_STACK_RED_ZONE;
    lo &= ~(uint64_t)7;
    uint64_t hi = c->stack_top;
    if (hi > lo && hi - lo > CV_STACK_SNAP_MAX)
        hi = lo + CV_STACK_SNAP_MAX;
    if (hi > lo) {
        size_t len = hi - lo;
        s->stack = malloc(len);
        if (s->stack) {
            read_mem_zerofill(c, lo, s->stack, len);
            s->stack_len = len;
            s->stack_base = lo;
        }
    }

    /* heap: [heap] mapping + tracked anonymous mmaps, shared budget */
    size_t budget = CV_HEAP_SNAP_MAX;
    range_t regions[CV_MAX_HEAPR];
    int nregions = 0;
    if (c->brk_heap.end > c->brk_heap.start)
        regions[nregions++] = c->brk_heap;
    for (int i = 0; i < c->n_anonr && nregions < CV_MAX_HEAPR; i++)
        regions[nregions++] = c->anonr[i];
    for (int i = 0; i < nregions && budget > 0; i++) {
        size_t share = budget / (size_t)(nregions - i); /* fair split */
        size_t len = regions[i].end - regions[i].start;
        if (len > share)
            len = share;
        uint8_t *buf = malloc(len);
        if (!buf)
            break;
        read_mem_zerofill(c, regions[i].start, buf, len);
        heapreg_t *hr = &s->heapr[s->n_heapr++];
        hr->buf = buf;
        hr->base = regions[i].start;
        hr->len = len;
        budget -= len;
    }

    /* globals: .data + .bss covering range (fixed, from ELF) */
    if (t->globals_rng.end > t->globals_rng.start) {
        size_t len = t->globals_rng.end - t->globals_rng.start;
        s->globals = malloc(len);
        if (s->globals) {
            read_mem_zerofill(c, t->globals_rng.start, s->globals, len);
            s->globals_len = len;
        }
    }

    s->insn_idx = trace_insn_lookup(t, regs->rip);
    s->src_line = trace_line_lookup(t, regs->rip);
    return 0;
}

/* ---------------- syscall handling ---------------- */

/* record the event and apply side effects that change what we snapshot */
static void handle_syscall(rec_ctx_t *c, trace_t *t, const pending_sc_t *p,
                           int64_t ret, int recording)
{
    if (recording) {
        if (t->n_scs == t->cap_scs) {
            size_t ncap = t->cap_scs ? t->cap_scs * 2 : 64;
            scevent_t *ns = realloc(t->scs, ncap * sizeof(scevent_t));
            if (ns) {
                t->scs = ns;
                t->cap_scs = ncap;
            }
        }
        if (t->n_scs < t->cap_scs) {
            scevent_t *e = &t->scs[t->n_scs++];
            e->step = t->n_steps ? t->n_steps - 1 : 0;
            e->nr = p->nr;
            e->ret = ret;
            memcpy(e->args, p->args, sizeof(e->args));
        }
    }

    switch (p->nr) {
    case 12: /* brk: [heap] may have just appeared or grown */
        maps_find(c->pid, "[heap]", &c->brk_heap);
        break;
    case 9: { /* mmap: track anonymous writable mappings (big mallocs);
               * only while recording, so loader-internal mmaps stay out */
        uint64_t uret = (uint64_t)ret;
        int anon     = (p->args[3] & 0x20) != 0; /* MAP_ANONYMOUS */
        int writable = (p->args[2] & 0x2)  != 0; /* PROT_WRITE */
        if (recording && uret < 0xfffffffffffff000ULL && anon && writable &&
            p->args[1] > 0 && p->args[1] <= CV_MMAP_TRACK_MAX &&
            c->n_anonr < CV_MAX_HEAPR - 1) {
            uint64_t len = (p->args[1] + 4095) & ~4095ULL;
            c->anonr[c->n_anonr++] = (range_t){ uret, uret + len };
        }
        break;
    }
    case 11: { /* munmap: forget tracked regions inside the range */
        uint64_t lo = p->args[0], hi = p->args[0] + p->args[1];
        for (int i = 0; i < c->n_anonr; ) {
            if (c->anonr[i].start >= lo && c->anonr[i].end <= hi)
                c->anonr[i] = c->anonr[--c->n_anonr];
            else
                i++;
        }
        break;
    }
    case 56: case 57: case 58: case 59: case 435: /* clone/fork/vfork/execve */
        if (t->fork_step < 0 && recording)
            t->fork_step = (int64_t)(t->n_steps ? t->n_steps - 1 : 0);
        break;
    }
}

/* ---------------- main record loop ---------------- */

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
        /* child */
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

    rec_ctx_t c = {
        .pid = pid, .mem_fd = -1, .use_pvr = 1, .out_fd = pfd[0],
    };
    t->fork_step = -1;

    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "cvisor: target failed to start\n");
        close(pfd[0]);
        return -1;
    }
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_EXITKILL);

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) {
        perror("cvisor: PTRACE_GETREGS");
        goto kill_out;
    }
    c.stack_top = regs.rsp + CV_STACK_ABOVE_RSP0;

    if (from_main && t->main_addr == 0) {
        fprintf(stderr, "cvisor: --from-main: 'main' symbol not found; "
                        "recording from _start\n");
        from_main = 0;
    }

    int recording = !from_main;
    size_t total_singlesteps = 0;
    uint32_t skipped_since = 0;
    pending_sc_t psc = { 0, 0, {0} };
    int deliver_sig = 0; /* pending non-fatal signal to hand to the tracee */
    int skip_pre = 0;    /* current insn already snapshotted (signal retry) */
    fprintf(stderr, "Recording... ");

    for (;;) {
        if (skip_pre) {
            skip_pre = 0;
            goto sc_check; /* insn already snapshotted; re-arm syscall check */
        }

        /* refresh [heap] periodically (belt and braces; brk also triggers) */
        if (total_singlesteps % CV_HEAP_RECHECK == 0)
            maps_find(pid, "[heap]", &c.brk_heap);

        if (!recording && regs.rip == t->main_addr)
            recording = 1;

        if (recording &&
            regs.rip >= t->text.start && regs.rip < t->text.end) {
            if (t->n_steps >= max_steps) {
                t->truncated = 1;
                fprintf(stderr, "\rRecording... %zu steps (cap reached, "
                        "truncated)\n", t->n_steps);
                goto kill_out_ok;
            }
            if (snapshot(&c, t, &regs) < 0) {
                fprintf(stderr, "\ncvisor: out of memory at step %zu\n",
                        t->n_steps);
                t->truncated = 1;
                goto kill_out_ok;
            }
            t->steps[t->n_steps - 1].skipped = skipped_since;
            skipped_since = 0;
            drain_output(&c, t, t->n_steps - 1);
            if (t->n_steps % 500 == 0)
                fprintf(stderr, "\rRecording... %zu steps", t->n_steps);
        } else if (recording) {
            skipped_since++; /* executing but hidden (libc internals) */
        }

sc_check:
        /* is the instruction we are about to execute a `syscall` (0f 05)? */
        psc.active = 0;
        uint8_t op[2];
        if (read_mem(&c, regs.rip, op, 2) == 2 &&
            op[0] == 0x0f && op[1] == 0x05) {
            psc.active = 1;
            psc.nr = (int64_t)regs.rax;
            psc.args[0] = regs.rdi; psc.args[1] = regs.rsi;
            psc.args[2] = regs.rdx; psc.args[3] = regs.r10;
            psc.args[4] = regs.r8;  psc.args[5] = regs.r9;
        }

        if (ptrace(PTRACE_SINGLESTEP, pid, 0, deliver_sig) < 0) {
            perror("\ncvisor: PTRACE_SINGLESTEP");
            goto kill_out;
        }
        deliver_sig = 0;
        total_singlesteps++;
        if (waitpid(pid, &status, 0) < 0) {
            perror("\ncvisor: waitpid");
            goto kill_out;
        }

        if (WIFEXITED(status)) {
            t->exit_code = WEXITSTATUS(status);
            break;
        }
        if (WIFSIGNALED(status)) {
            t->death_signal = WTERMSIG(status);
            break;
        }
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            if (sig != SIGTRAP) {
                /* the instruction did NOT execute; drop any pending syscall */
                psc.active = 0;
                if (sig == SIGCHLD || sig == SIGWINCH ||
                    sig == SIGURG || sig == SIGCONT) {
                    /* harmless: deliver it and retry the same instruction
                     * (rip is unchanged, so skip re-snapshotting it) */
                    deliver_sig = sig;
                    skip_pre = 1;
                    continue;
                }
                /* fatal (e.g. SIGSEGV): snapshot the faulting state,
                 * then let the target die with it */
                t->death_signal = sig;
                if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0 &&
                    t->n_steps < max_steps)
                    snapshot(&c, t, &regs);
                ptrace(PTRACE_KILL, pid, 0, 0);
                waitpid(pid, &status, 0);
                break;
            }
        }

        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) {
            perror("\ncvisor: PTRACE_GETREGS");
            goto kill_out;
        }

        if (psc.active) /* the syscall just completed; RAX = return value */
            handle_syscall(&c, t, &psc, (int64_t)regs.rax, recording);
    }

    drain_output(&c, t, t->n_steps ? t->n_steps - 1 : 0);
    fprintf(stderr, "\rRecording... %zu steps%s\n", t->n_steps,
            t->death_signal ? " (target killed by signal)" : " (done)");
    if (c.mem_fd >= 0)
        close(c.mem_fd);
    close(pfd[0]);
    if (t->n_steps == 0) {
        if (t->exit_code == 127)
            fprintf(stderr, "cvisor: exec failed — is '%s' executable?\n",
                    target_path);
        else
            fprintf(stderr, "cvisor: no steps recorded (nothing to show)\n");
        return -1;
    }
    return 0;

kill_out_ok:
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    drain_output(&c, t, t->n_steps ? t->n_steps - 1 : 0);
    if (c.mem_fd >= 0)
        close(c.mem_fd);
    close(pfd[0]);
    return t->n_steps > 0 ? 0 : -1;

kill_out:
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    if (c.mem_fd >= 0)
        close(c.mem_fd);
    close(pfd[0]);
    return -1;
}

/* ---------------- --trace text dump (Phase 1 verification) ---------------- */

void record_dump(const trace_t *t)
{
    size_t sc = 0;
    for (size_t i = 0; i < t->n_steps; i++) {
        const step_t *s = &t->steps[i];
        const char *insn = (s->insn_idx >= 0)
                               ? t->dlines[s->insn_idx].text : "?";
        printf("step %zu | rip=0x%llx | %s:%d | %s",
               i, (unsigned long long)s->regs.rip,
               t->src_file[0] ? t->src_file : "?", s->src_line, insn);
        if (s->skipped)
            printf(" | +%u libc insns", s->skipped);
        if (i > 0) {
            const step_t *p = &t->steps[i - 1];
            for (int r = 0; r < CV_NREGS; r++) {
                uint64_t ov = cv_reg(&p->regs, r);
                uint64_t nv = cv_reg(&s->regs, r);
                if (ov != nv && strcmp(CV_REGS[r].name, "RIP") != 0)
                    printf(" | %s %llx->%llx", CV_REGS[r].name,
                           (unsigned long long)ov, (unsigned long long)nv);
            }
        }
        printf("\n");

        for (; sc < t->n_scs && t->scs[sc].step == i; sc++) {
            const scevent_t *e = &t->scs[sc];
            const char *name = cv_syscall_name(e->nr);
            if (name)
                printf("    syscall %s(", name);
            else
                printf("    syscall sys_%lld(", (long long)e->nr);
            printf("0x%llx, 0x%llx, 0x%llx) = %lld\n",
                   (unsigned long long)e->args[0],
                   (unsigned long long)e->args[1],
                   (unsigned long long)e->args[2], (long long)e->ret);
        }
    }
    if (t->n_steps) {
        size_t widest = 0;
        for (size_t i = 0; i < t->n_steps; i++)
            if (t->steps[i].n_heapr > t->steps[widest].n_heapr)
                widest = i;
        size_t show[2] = { widest, t->n_steps - 1 };
        for (int k = 0; k < 2; k++) {
            const step_t *s = &t->steps[show[k]];
            if (k == 1 && show[1] == show[0])
                break;
            printf("== heap regions at step %zu:", show[k]);
            for (int r = 0; r < s->n_heapr; r++)
                printf(" %s0x%llx(%zu bytes)", r == 0 ? "" : "mmap:",
                       (unsigned long long)s->heapr[r].base,
                       s->heapr[r].len);
            if (s->n_heapr == 0)
                printf(" none");
            printf(" ==\n");
        }
    }
    if (t->fork_step >= 0)
        printf("== WARNING: fork/clone/exec at step %lld — child processes "
               "are not followed (Phase 3) ==\n", (long long)t->fork_step);
    printf("== %zu steps | exit: ", t->n_steps);
    if (t->death_signal)
        printf("killed by signal %d (%s)", t->death_signal,
               strsignal(t->death_signal));
    else
        printf("code %d", t->exit_code);
    if (t->truncated)
        printf(" | TRUNCATED at cap");
    printf(" ==\n");
    if (t->out_len) {
        printf("== program output (%zu bytes) ==\n", t->out_len);
        fwrite(t->prog_output, 1, t->out_len, stdout);
        if (t->prog_output[t->out_len - 1] != '\n')
            printf("\n");
    }
}
