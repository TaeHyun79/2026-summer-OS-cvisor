/* trace.c - trace storage, lookups, register/syscall tables */
#define _GNU_SOURCE
#include "cvisor.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* register display order: pointers first, then GPRs, then eflags */
const regdesc_t CV_REGS[] = {
    { "RIP", offsetof(struct user_regs_struct, rip) },
    { "RSP", offsetof(struct user_regs_struct, rsp) },
    { "RBP", offsetof(struct user_regs_struct, rbp) },
    { "RAX", offsetof(struct user_regs_struct, rax) },
    { "RBX", offsetof(struct user_regs_struct, rbx) },
    { "RCX", offsetof(struct user_regs_struct, rcx) },
    { "RDX", offsetof(struct user_regs_struct, rdx) },
    { "RSI", offsetof(struct user_regs_struct, rsi) },
    { "RDI", offsetof(struct user_regs_struct, rdi) },
    { "R8 ", offsetof(struct user_regs_struct, r8)  },
    { "R9 ", offsetof(struct user_regs_struct, r9)  },
    { "R10", offsetof(struct user_regs_struct, r10) },
    { "R11", offsetof(struct user_regs_struct, r11) },
    { "R12", offsetof(struct user_regs_struct, r12) },
    { "R13", offsetof(struct user_regs_struct, r13) },
    { "R14", offsetof(struct user_regs_struct, r14) },
    { "R15", offsetof(struct user_regs_struct, r15) },
    { "FLG", offsetof(struct user_regs_struct, eflags) },
};
const int CV_NREGS = (int)(sizeof(CV_REGS) / sizeof(CV_REGS[0]));

uint64_t cv_reg(const struct user_regs_struct *r, int i)
{
    uint64_t v;
    memcpy(&v, (const char *)r + CV_REGS[i].off, sizeof(v));
    return v;
}

/* x86-64 syscall numbers a student program is likely to hit */
static const struct { int64_t nr; const char *name; } SC_NAMES[] = {
    {0,"read"},{1,"write"},{2,"open"},{3,"close"},{4,"stat"},{5,"fstat"},
    {8,"lseek"},{9,"mmap"},{10,"mprotect"},{11,"munmap"},{12,"brk"},
    {13,"rt_sigaction"},{14,"rt_sigprocmask"},{16,"ioctl"},{20,"writev"},
    {21,"access"},{22,"pipe"},{28,"madvise"},{32,"dup"},{33,"dup2"},
    {35,"nanosleep"},{39,"getpid"},{56,"clone"},{57,"fork"},{58,"vfork"},
    {59,"execve"},{60,"exit"},{61,"wait4"},{62,"kill"},{63,"uname"},
    {72,"fcntl"},{79,"getcwd"},{89,"readlink"},{96,"gettimeofday"},
    {102,"getuid"},{104,"getgid"},{107,"geteuid"},{108,"getegid"},
    {158,"arch_prctl"},{186,"gettid"},{201,"time"},{202,"futex"},
    {218,"set_tid_address"},{228,"clock_gettime"},{231,"exit_group"},
    {257,"openat"},{262,"newfstatat"},{273,"set_robust_list"},
    {302,"prlimit64"},{318,"getrandom"},{334,"rseq"},{435,"clone3"},
};

const char *cv_syscall_name(int64_t nr)
{
    for (size_t i = 0; i < sizeof(SC_NAMES) / sizeof(SC_NAMES[0]); i++)
        if (SC_NAMES[i].nr == nr)
            return SC_NAMES[i].name;
    return NULL;
}

/* exact binary search over irefs (rip is always an instruction boundary) */
int32_t img_insn_lookup(const image_t *im, uint64_t rip)
{
    size_t lo = 0, hi = im->n_irefs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (im->irefs[mid].addr < rip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < im->n_irefs && im->irefs[lo].addr == rip)
        return im->irefs[lo].dline_idx;
    return -1;
}

/* greatest lmap entry with addr <= rip */
int32_t img_line_lookup(const image_t *im, uint64_t rip)
{
    if (im->n_lmap == 0)
        return -1;
    size_t lo = 0, hi = im->n_lmap;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (im->lmap[mid].addr <= rip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return -1;
    return im->lmap[lo - 1].line;
}

step_t *proc_new_step(proc_t *p)
{
    if (p->n_steps == p->cap_steps) {
        size_t ncap = p->cap_steps ? p->cap_steps * 2 : 1024;
        step_t *ns = realloc(p->steps, ncap * sizeof(step_t));
        if (!ns)
            return NULL;
        p->steps = ns;
        p->cap_steps = ncap;
    }
    step_t *s = &p->steps[p->n_steps++];
    memset(s, 0, sizeof(*s));
    s->src_line = -1;
    s->insn_idx = -1;
    return s;
}

void trace_append_output(trace_t *t, const char *buf, size_t len)
{
    if (t->out_len + len > t->out_cap) {
        size_t ncap = t->out_cap ? t->out_cap * 2 : 4096;
        while (ncap < t->out_len + len)
            ncap *= 2;
        char *nb = realloc(t->prog_output, ncap);
        if (!nb)
            return;
        t->prog_output = nb;
        t->out_cap = ncap;
    }
    memcpy(t->prog_output + t->out_len, buf, len);
    t->out_len += len;
}

void trace_free(trace_t *t)
{
    for (int pi = 0; pi < t->n_procs; pi++) {
        proc_t *p = &t->procs[pi];
        for (size_t i = 0; i < p->n_steps; i++) {
            free(p->steps[i].stack);
            for (int r = 0; r < p->steps[i].n_heapr; r++)
                free(p->steps[i].heapr[r].buf);
            free(p->steps[i].globals);
        }
        free(p->steps);
    }
    for (int i = 0; i < t->n_images; i++)
        image_free(t->images[i]);
    free(t->prog_output);
    free(t->chunks);
    free(t->scs);
    memset(t, 0, sizeof(*t));
}
