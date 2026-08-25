/* tui.c - ncurses replay UI (spec 6.3)
 *
 * Reads only the immutable trace + static analysis results; stepping
 * forward/backward is just moving an index into trace->steps.
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <ctype.h>
#include <curses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

enum { PANE_STACK = 0, PANE_HEAP, PANE_GLOBALS, PANE_RODATA, PANE_CODE,
       PANE_N };

/* wide layout: 4x2 grid with every memory section visible at once.
 * Kicks in when the terminal is at least this big — roughly 2/3 of a
 * typical fullscreen terminal, and wide enough that each of the 4 columns
 * fits a hex row (12 addr + 24 hex + borders).  The true screen maximum is
 * not queryable from inside a terminal, so this is a fixed threshold. */
#define WIDE_MIN_COLS  160
#define WIDE_MIN_LINES 34

enum {
    CP_CUR = 1,   /* current source line / instruction */
    CP_CHG,       /* value changed vs previous step */
    CP_MARK,      /* RSP/RBP markers */
    CP_TITLE,     /* panel titles */
    CP_BAD,       /* death signal in status bar */
};

typedef struct {
    trace_t *t;
    size_t   cur;
    int      mode_src;              /* 0 = instruction step, 1 = source step */
    int      mem_pane;
    int      mem_scroll[PANE_N];    /* row offset per memory pane */
    int      stack_desc;            /* 1 = high addresses on top (CSAPP) */
    int      show_out;
    int      show_sys;              /* syscall log overlay */
    int      wide;                  /* 4x2 layout: all memory panes shown */
    WINDOW  *wsrc, *wasm, *wreg, *wmem, *wout, *wsys;
    WINDOW  *wmems[PANE_N];         /* wide mode: one window per section */
} ui_t;

/* ---------------- helpers ---------------- */

static const step_t *cur_step(ui_t *u)  { return &u->t->steps[u->cur]; }
static const step_t *prev_step(ui_t *u)
{
    return &u->t->steps[u->cur > 0 ? u->cur - 1 : 0];
}

/* fetch the byte at absolute address `addr` from a step's snapshots */
static int step_byte(const trace_t *t, const step_t *s, uint64_t addr,
                     uint8_t *out)
{
    if (s->stack && addr >= s->stack_base &&
        addr < s->stack_base + s->stack_len) {
        *out = s->stack[addr - s->stack_base];
        return 0;
    }
    for (int i = 0; i < s->n_heapr; i++) {
        const heapreg_t *hr = &s->heapr[i];
        if (hr->buf && addr >= hr->base && addr < hr->base + hr->len) {
            *out = hr->buf[addr - hr->base];
            return 0;
        }
    }
    if (s->globals && addr >= t->globals_rng.start &&
        addr < t->globals_rng.start + s->globals_len) {
        *out = s->globals[addr - t->globals_rng.start];
        return 0;
    }
    return -1;
}

static void draw_frame_f(WINDOW *w, const char *title, int focus)
{
    werase(w);
    box(w, 0, 0);
    if (title) {
        int attr = COLOR_PAIR(CP_TITLE) | A_BOLD | (focus ? A_REVERSE : 0);
        wattron(w, attr);
        mvwprintw(w, 0, 2, " %.*s ", getmaxx(w) - 6, title);
        wattroff(w, attr);
    }
}

static void draw_frame(WINDOW *w, const char *title)
{
    draw_frame_f(w, title, 0);
}

/* first index so that `pos` stays roughly centered in `visible` rows */
static int center_first(int pos, int total, int visible)
{
    if (total <= visible)
        return 0;
    int first = pos - visible / 2;
    if (first < 0)
        first = 0;
    if (first > total - visible)
        first = total - visible;
    return first;
}

/* ---------------- source panel ---------------- */

static void draw_src(ui_t *u)
{
    trace_t *t = u->t;
    char title[600];
    snprintf(title, sizeof(title), "%s",
             t->src_file[0] ? t->src_file : "source");
    draw_frame(u->wsrc, title);

    int h = getmaxy(u->wsrc) - 2, w = getmaxx(u->wsrc) - 2;
    if (h <= 0)
        return;
    if (t->n_src == 0) {
        mvwprintw(u->wsrc, 1, 2, "(source not available)");
        wnoutrefresh(u->wsrc);
        return;
    }

    int cur_line = cur_step(u)->src_line; /* 1-based, -1 = none */
    int pos = (cur_line > 0 ? cur_line : 1) - 1;
    int first = center_first(pos, t->n_src, h);

    for (int i = 0; i < h && first + i < t->n_src; i++) {
        int lineno = first + i + 1;
        int is_cur = (lineno == cur_line);
        if (is_cur)
            wattron(u->wsrc, COLOR_PAIR(CP_CUR) | A_BOLD);
        mvwprintw(u->wsrc, 1 + i, 1, "%c%4d  %-.*s",
                  is_cur ? '>' : ' ', lineno,
                  w > 7 ? w - 7 : 0, t->src[first + i]);
        if (is_cur) {
            /* pad highlight to panel edge */
            int x = getcurx(u->wsrc);
            for (; x < w + 1; x++)
                mvwaddch(u->wsrc, 1 + i, x, ' ');
            wattroff(u->wsrc, COLOR_PAIR(CP_CUR) | A_BOLD);
        }
    }
    wnoutrefresh(u->wsrc);
}

/* ---------------- disassembly panel ---------------- */

static void draw_asm(ui_t *u)
{
    trace_t *t = u->t;
    draw_frame(u->wasm, "disassembly");
    int h = getmaxy(u->wasm) - 2, w = getmaxx(u->wasm) - 2;
    if (h <= 0)
        return;

    int cur = cur_step(u)->insn_idx;
    int first = center_first(cur >= 0 ? cur : 0, (int)t->n_dlines, h);

    for (int i = 0; i < h && (size_t)(first + i) < t->n_dlines; i++) {
        const dline_t *d = &t->dlines[first + i];
        int is_cur = (first + i == cur);
        if (is_cur)
            wattron(u->wasm, COLOR_PAIR(CP_CUR) | A_BOLD);
        if (d->is_label)
            mvwprintw(u->wasm, 1 + i, 1, " %-.*s", w - 1, d->text);
        else
            mvwprintw(u->wasm, 1 + i, 1, "%c%llx: %-.*s",
                      is_cur ? '>' : ' ',
                      (unsigned long long)d->addr,
                      w > 10 ? w - 10 : 0, d->text);
        if (is_cur) {
            int x = getcurx(u->wasm);
            for (; x < w + 1; x++)
                mvwaddch(u->wasm, 1 + i, x, ' ');
            wattroff(u->wasm, COLOR_PAIR(CP_CUR) | A_BOLD);
        }
    }
    wnoutrefresh(u->wasm);
}

/* ---------------- register panel ---------------- */

static const struct { const char *name; int bit; } FLAG_BITS[] = {
    { "CF", 0 }, { "PF", 2 }, { "AF", 4 }, { "ZF", 6 },
    { "SF", 7 }, { "DF", 10 }, { "OF", 11 },
};
#define N_FLAG_BITS ((int)(sizeof(FLAG_BITS) / sizeof(FLAG_BITS[0])))

static void draw_reg(ui_t *u)
{
    draw_frame(u->wreg, "registers");
    int h = getmaxy(u->wreg) - 2, w = getmaxx(u->wreg) - 2;
    if (h <= 1)
        return;

    const step_t *s = cur_step(u), *p = prev_step(u);
    int cell = 3 + 1 + 16 + 3;                 /* "RAX 0123456789abcdef * " */
    int ncols = w / cell > 0 ? w / cell : 1;
    int rows = h - 1;                          /* last row: decoded flags */
    if (rows < 1)
        rows = 1;

    for (int i = 0; i < CV_NREGS; i++) {
        int col = i / rows, row = i % rows;
        if (col >= ncols)
            break;
        uint64_t v = cv_reg(&s->regs, i), pv = cv_reg(&p->regs, i);
        int changed = (u->cur > 0 && v != pv &&
                       strcmp(CV_REGS[i].name, "RIP") != 0);
        int x = 1 + col * cell;
        mvwprintw(u->wreg, 1 + row, x, "%s ", CV_REGS[i].name);
        if (changed)
            wattron(u->wreg, COLOR_PAIR(CP_CHG) | A_BOLD);
        wprintw(u->wreg, "%016llx", (unsigned long long)v);
        if (changed) {
            wprintw(u->wreg, "*");
            wattroff(u->wreg, COLOR_PAIR(CP_CHG) | A_BOLD);
        }
    }

    /* decoded EFLAGS on the last row (spec 7.3) */
    uint64_t fl = s->regs.eflags, pfl = p->regs.eflags;
    mvwprintw(u->wreg, h, 1, "FLAGS:");
    for (int i = 0; i < N_FLAG_BITS; i++) {
        int set = (fl >> FLAG_BITS[i].bit) & 1;
        int was = (pfl >> FLAG_BITS[i].bit) & 1;
        int changed = (u->cur > 0 && set != was);
        if (changed)
            wattron(u->wreg, COLOR_PAIR(CP_CHG) | A_BOLD);
        else if (set)
            wattron(u->wreg, A_BOLD);
        else
            wattron(u->wreg, A_DIM);
        wprintw(u->wreg, " %s%c", FLAG_BITS[i].name, set ? '1' : '0');
        wattroff(u->wreg, COLOR_PAIR(CP_CHG) | A_BOLD | A_DIM);
    }
    wnoutrefresh(u->wreg);
}

/* ---------------- memory panel ---------------- */

static void mem_region(ui_t *u, int pane, uint64_t *base, size_t *len,
                       const uint8_t **buf, const char **name)
{
    const step_t *s = cur_step(u);
    const trace_t *t = u->t;
    switch (pane) {
    case PANE_STACK:
        *base = s->stack_base; *len = s->stack_len; *buf = s->stack;
        *name = "stack";
        break;
    case PANE_RODATA: /* read-only: loaded once from the ELF, never changes */
        *base = t->rodata_rng.start;
        *len  = t->rodata_bytes ? t->rodata_rng.end - t->rodata_rng.start : 0;
        *buf  = t->rodata_bytes;
        *name = "rodata (string literals, constants)";
        break;
    case PANE_CODE:
        *base = t->text.start;
        *len  = t->text_bytes ? t->text.end - t->text.start : 0;
        *buf  = t->text_bytes;
        *name = "code (.text machine bytes)";
        break;
    default:
        *base = t->globals_rng.start; *len = s->globals_len;
        *buf = s->globals; *name = "globals (.got/.data/.bss)";
        break;
    }
}

/* one hex row of 8 bytes with per-byte change highlighting and an ASCII
 * column; sbuf != NULL means a static section (bytes from the ELF file,
 * never change) instead of per-step snapshots */
static void draw_hex_row(ui_t *u, WINDOW *w, int y, uint64_t row_addr,
                         uint64_t base, size_t len, const uint8_t *sbuf)
{
    const step_t *s = cur_step(u), *p = prev_step(u);
    char ascii[9] = "        ";
    mvwprintw(w, y, 1, "%012llx ", (unsigned long long)row_addr);
    for (int b = 0; b < 8; b++) {
        uint64_t a = row_addr + (uint64_t)b;
        uint8_t v;
        int have, changed = 0;
        if (a < base || a >= base + len) {
            have = 0;
        } else if (sbuf) {
            v = sbuf[a - base];
            have = 1;
        } else {
            have = (step_byte(u->t, s, a, &v) == 0);
            uint8_t pv;
            changed = (have && u->cur > 0 &&
                       (step_byte(u->t, p, a, &pv) < 0 || pv != v));
        }
        if (!have) {
            wprintw(w, " ..");
            continue;
        }
        ascii[b] = (v >= 0x20 && v < 0x7f) ? (char)v : '.';
        if (changed)
            wattron(w, COLOR_PAIR(CP_CHG) | A_BOLD);
        wprintw(w, " %02x", v);
        if (changed)
            wattroff(w, COLOR_PAIR(CP_CHG) | A_BOLD);
    }
    if (getmaxx(w) - 2 >= 13 + 24 + 2 + 8)
        wprintw(w, "  %s", ascii);
}

/* heap pane: [heap] plus tracked anonymous mmap regions, stacked */
static void draw_mem_heap(ui_t *u, WINDOW *w, int focus)
{
    const step_t *s = cur_step(u);
    draw_frame_f(w, u->wide ? "heap (brk+mmap)"
                            : "heap (brk+mmap)  (Tab: next pane)", focus);
    int h = getmaxy(w) - 2;
    if (h <= 0)
        return;
    if (s->n_heapr == 0) {
        mvwprintw(w, 1, 2, "(no heap region at this step)");
        wnoutrefresh(w);
        return;
    }

    /* virtual rows: per region one header row + its hex rows */
    int reg_rows[CV_MAX_HEAPR], reg_first[CV_MAX_HEAPR], total = 0;
    for (int i = 0; i < s->n_heapr; i++) {
        reg_first[i] = total;
        reg_rows[i] = 1 + (int)((s->heapr[i].len + 7) / 8);
        total += reg_rows[i];
    }
    int first = u->mem_scroll[PANE_HEAP];
    if (first > total - h)
        first = total - h;
    if (first < 0)
        first = 0;
    u->mem_scroll[PANE_HEAP] = first;

    for (int i = 0; i < h && first + i < total; i++) {
        int vrow = first + i, r = 0;
        while (r + 1 < s->n_heapr && reg_first[r + 1] <= vrow)
            r++;
        const heapreg_t *hr = &s->heapr[r];
        int inner = vrow - reg_first[r];
        if (inner == 0) {
            wattron(w, COLOR_PAIR(CP_MARK) | A_BOLD);
            mvwprintw(w, 1 + i, 1, "== %s 0x%llx (%zu bytes) ==",
                      r == 0 ? "region" : "mmap",
                      (unsigned long long)hr->base, hr->len);
            wattroff(w, COLOR_PAIR(CP_MARK) | A_BOLD);
        } else {
            uint64_t row_addr = hr->base + (uint64_t)(inner - 1) * 8;
            draw_hex_row(u, w, 1 + i, row_addr, hr->base, hr->len, NULL);
        }
    }
    wnoutrefresh(w);
}

/* draw one memory section into the given window; `focus` highlights the
 * pane that Tab has selected for scrolling (wide mode) */
static void draw_mem_one(ui_t *u, WINDOW *w, int pane, int focus)
{
    if (pane == PANE_HEAP) {
        draw_mem_heap(u, w, focus);
        return;
    }
    uint64_t base; size_t len; const uint8_t *buf; const char *name;
    mem_region(u, pane, &base, &len, &buf, &name);

    char title[80];
    snprintf(title, sizeof(title), "%s%s", name,
             u->wide ? "" : "  (Tab: next pane)");
    draw_frame_f(w, title, focus);

    int h = getmaxy(w) - 2;
    if (h <= 0)
        return;
    if (!buf || len == 0) {
        mvwprintw(w, 1, 2, "(no %s region at this step)", name);
        wnoutrefresh(w);
        return;
    }

    const step_t *s = cur_step(u);
    uint64_t lo = base & ~(uint64_t)7;
    uint64_t hi = (base + len + 7) & ~(uint64_t)7;
    int nrows = (int)((hi - lo) / 8);

    /* centering anchor: RSP row for the stack, RIP row for code */
    int anchor = 0;
    if (pane == PANE_STACK && s->regs.rsp >= lo && s->regs.rsp < hi)
        anchor = (int)((s->regs.rsp - lo) / 8);
    if (pane == PANE_CODE && s->regs.rip >= lo && s->regs.rip < hi)
        anchor = (int)((s->regs.rip - lo) / 8);
    int descending = (pane == PANE_STACK && u->stack_desc);
    int anchor_disp = descending ? nrows - 1 - anchor : anchor;

    int first = center_first(anchor_disp, nrows, h) + u->mem_scroll[pane];
    if (first > nrows - h)
        first = nrows - h;
    if (first < 0)
        first = 0;
    u->mem_scroll[pane] = first - center_first(anchor_disp, nrows, h);

    for (int i = 0; i < h && first + i < nrows; i++) {
        int disp = first + i;
        int rowi = descending ? nrows - 1 - disp : disp;
        uint64_t row_addr = lo + (uint64_t)rowi * 8;

        const uint8_t *sbuf =
            (pane == PANE_CODE)   ? u->t->text_bytes :
            (pane == PANE_RODATA) ? u->t->rodata_bytes : NULL;
        draw_hex_row(u, w, 1 + i, row_addr, base, len, sbuf);

        /* RSP/RBP markers on the stack, RIP marker on the code bytes */
        if (pane == PANE_STACK) {
            int has_rsp = s->regs.rsp >= row_addr && s->regs.rsp < row_addr + 8;
            int has_rbp = s->regs.rbp >= row_addr && s->regs.rbp < row_addr + 8;
            if (has_rsp || has_rbp) {
                wattron(w, COLOR_PAIR(CP_MARK) | A_BOLD);
                wprintw(w, " <-%s%s%s",
                        has_rsp ? "RSP" : "",
                        (has_rsp && has_rbp) ? "," : "",
                        has_rbp ? "RBP" : "");
                wattroff(w, COLOR_PAIR(CP_MARK) | A_BOLD);
            }
        } else if (pane == PANE_CODE &&
                   s->regs.rip >= row_addr && s->regs.rip < row_addr + 8) {
            wattron(w, COLOR_PAIR(CP_MARK) | A_BOLD);
            wprintw(w, " <-RIP");
            wattroff(w, COLOR_PAIR(CP_MARK) | A_BOLD);
        }
    }
    wnoutrefresh(w);
}

/* ---------------- output overlay ---------------- */

static size_t output_len_at(const trace_t *t, size_t step)
{
    size_t n = 0;
    for (size_t i = 0; i < t->n_chunks; i++)
        if (t->chunks[i].step <= step)
            n = t->chunks[i].off + t->chunks[i].len;
    return n;
}

static void draw_out(ui_t *u)
{
    if (!u->wout)
        return;
    draw_frame(u->wout, "program output (o: close)");
    int h = getmaxy(u->wout) - 2, w = getmaxx(u->wout) - 2;
    if (h <= 0)
        return;

    size_t len = output_len_at(u->t, u->cur);
    const char *out = u->t->prog_output;

    /* split into lines, keep only the last h */
    const char *lines[512];
    int nlines = 0;
    const char *pstart = out;
    for (size_t i = 0; i < len; i++) {
        if (out[i] == '\n') {
            if (nlines < 512)
                lines[nlines++] = pstart;
            else {
                memmove((void *)lines, (void *)(lines + 1),
                        511 * sizeof(char *));
                lines[511] = pstart;
            }
            pstart = out + i + 1;
        }
    }
    int partial = (pstart < out + len);
    int total = nlines + partial;
    int first = total > h ? total - h : 0;

    for (int i = first; i < total; i++) {
        const char *ls;
        size_t ll;
        if (i < nlines) {
            ls = lines[i];
            const char *e = memchr(ls, '\n', len - (size_t)(ls - out));
            ll = e ? (size_t)(e - ls) : len - (size_t)(ls - out);
        } else {
            ls = pstart;
            ll = len - (size_t)(pstart - out);
        }
        if (ll > (size_t)w)
            ll = (size_t)w;
        mvwprintw(u->wout, 1 + (i - first), 1, "%.*s", (int)ll, ls);
    }
    if (len == 0)
        mvwprintw(u->wout, 1, 2, "(no output up to this step)");
    wnoutrefresh(u->wout);
}

/* ---------------- syscall log overlay ---------------- */

static void draw_sys(ui_t *u)
{
    if (!u->wsys)
        return;
    draw_frame(u->wsys, "syscalls up to this step (s: close)");
    int h = getmaxy(u->wsys) - 2, w = getmaxx(u->wsys) - 2;
    if (h <= 0)
        return;

    const trace_t *t = u->t;
    /* events with step <= cur; show the last h of them */
    size_t n = 0;
    while (n < t->n_scs && t->scs[n].step <= u->cur)
        n++;
    size_t firstev = n > (size_t)h ? n - (size_t)h : 0;

    if (n == 0)
        mvwprintw(u->wsys, 1, 2, "(no syscalls up to this step)");
    for (size_t i = firstev; i < n; i++) {
        const scevent_t *e = &t->scs[i];
        const char *name = cv_syscall_name(e->nr);
        char nbuf[24];
        if (!name) {
            snprintf(nbuf, sizeof(nbuf), "sys_%lld", (long long)e->nr);
            name = nbuf;
        }
        char line[256];
        snprintf(line, sizeof(line),
                 "step %5zu  %s(0x%llx, 0x%llx, 0x%llx) = %lld",
                 e->step, name,
                 (unsigned long long)e->args[0],
                 (unsigned long long)e->args[1],
                 (unsigned long long)e->args[2], (long long)e->ret);
        int is_cur = (e->step == u->cur);
        if (is_cur)
            wattron(u->wsys, A_BOLD);
        mvwprintw(u->wsys, 1 + (int)(i - firstev), 1, "%-.*s", w, line);
        if (is_cur)
            wattroff(u->wsys, A_BOLD);
    }
    wnoutrefresh(u->wsys);
}

/* ---------------- status bar ---------------- */

static void draw_status(ui_t *u)
{
    trace_t *t = u->t;
    move(LINES - 1, 0);
    clrtoeol();
    attron(A_REVERSE);

    char skipbuf[32] = "";
    if (t->steps[u->cur].skipped)
        snprintf(skipbuf, sizeof(skipbuf), " (+%u libc)",
                 t->steps[u->cur].skipped);
    char left[256];
    snprintf(left, sizeof(left),
             " step %zu/%zu%s | mode: %s | mem: %s%s",
             u->cur, t->n_steps - 1, skipbuf,
             u->mode_src ? "SRC " : "INSN",
             u->mem_pane == PANE_STACK ? "stack" :
             u->mem_pane == PANE_HEAP ? "heap" :
             u->mem_pane == PANE_GLOBALS ? "globals" :
             u->mem_pane == PANE_RODATA ? "rodata" : "code",
             t->truncated ? " | TRUNCATED" : "");
    mvprintw(LINES - 1, 0, "%-.*s", COLS, left);

    if (t->fork_step >= 0) {
        attron(COLOR_PAIR(CP_BAD) | A_BOLD);
        printw(" | fork@%lld: children not followed",
               (long long)t->fork_step);
        attroff(COLOR_PAIR(CP_BAD) | A_BOLD);
        attron(A_REVERSE);
    }
    if (t->death_signal) {
        attron(COLOR_PAIR(CP_BAD) | A_BOLD);
        printw(" | killed: signal %d", t->death_signal);
        attroff(COLOR_PAIR(CP_BAD) | A_BOLD);
        attron(A_REVERSE);
    } else {
        printw(" | exit %d", t->exit_code);
    }

    const char *help =
        " | </> step  m mode  Tab mem  ^/v scroll  g goto  f fn  o out  "
        "s sys  q quit";
    int x = getcurx(stdscr);
    if (x + (int)strlen(help) < COLS)
        printw("%s", help);
    /* pad */
    for (x = getcurx(stdscr); x < COLS - 1; x++)
        addch(' ');
    attroff(A_REVERSE);
    wnoutrefresh(stdscr);
}

/* ---------------- layout ---------------- */

static void destroy_windows(ui_t *u)
{
    if (u->wsrc) delwin(u->wsrc);
    if (u->wasm) delwin(u->wasm);
    if (u->wreg) delwin(u->wreg);
    if (u->wmem) delwin(u->wmem);
    if (u->wout) delwin(u->wout);
    if (u->wsys) delwin(u->wsys);
    u->wsrc = u->wasm = u->wreg = u->wmem = u->wout = u->wsys = NULL;
    for (int i = 0; i < PANE_N; i++) {
        if (u->wmems[i])
            delwin(u->wmems[i]);
        u->wmems[i] = NULL;
    }
}

static void make_windows(ui_t *u)
{
    destroy_windows(u);
    int usable = LINES - 1;
    u->wide = (COLS >= WIDE_MIN_COLS && LINES >= WIDE_MIN_LINES);

    if (u->wide) {
        /* 4x2: src | asm | reg | stack  //  heap | globals | rodata | code */
        int top_h = usable / 2;
        int bot_h = usable - top_h;
        int cw = COLS / 4;
        u->wsrc = newwin(top_h, cw, 0, 0);
        u->wasm = newwin(top_h, cw, 0, cw);
        u->wreg = newwin(top_h, cw, 0, 2 * cw);
        u->wmems[PANE_STACK]   = newwin(top_h, COLS - 3 * cw, 0, 3 * cw);
        u->wmems[PANE_HEAP]    = newwin(bot_h, cw, top_h, 0);
        u->wmems[PANE_GLOBALS] = newwin(bot_h, cw, top_h, cw);
        u->wmems[PANE_RODATA]  = newwin(bot_h, cw, top_h, 2 * cw);
        u->wmems[PANE_CODE]    = newwin(bot_h, COLS - 3 * cw, top_h, 3 * cw);
    } else {
        int top_h = usable * 55 / 100;
        if (top_h < 5)
            top_h = usable > 5 ? 5 : usable;
        int bot_h = usable - top_h;
        int left_w = COLS / 2;
        u->wsrc = newwin(top_h, left_w, 0, 0);
        u->wasm = newwin(top_h, COLS - left_w, 0, left_w);
        u->wreg = newwin(bot_h, left_w, top_h, 0);
        u->wmem = newwin(bot_h, COLS - left_w, top_h, left_w);
    }
    if (u->show_out) {
        int oh = usable / 2;
        u->wout = newwin(oh, COLS, usable - oh, 0);
    }
    if (u->show_sys) {
        int oh = usable / 2;
        u->wsys = newwin(oh, COLS, usable - oh, 0);
    }
}

static void redraw(ui_t *u)
{
    erase();
    wnoutrefresh(stdscr);
    draw_src(u);
    draw_asm(u);
    draw_reg(u);
    if (u->wide) {
        for (int p = 0; p < PANE_N; p++)
            draw_mem_one(u, u->wmems[p], p, p == u->mem_pane);
    } else {
        draw_mem_one(u, u->wmem, u->mem_pane, 0);
    }
    if (u->show_out)
        draw_out(u);
    if (u->show_sys)
        draw_sys(u);
    draw_status(u);
    doupdate();
}

/* ---------------- stepping ---------------- */

/* source-line mode: first index (in direction dir) whose valid src_line
 * differs from the current line (spec 6.3 "스텝 모드 구현") */
static size_t src_step(trace_t *t, size_t cur, int dir)
{
    int32_t line = t->steps[cur].src_line;
    long i = (long)cur + dir;
    while (i >= 0 && i < (long)t->n_steps) {
        int32_t l = t->steps[i].src_line;
        if (l != -1 && l != line)
            return (size_t)i;
        i += dir;
    }
    return dir > 0 ? t->n_steps - 1 : 0;
}

static size_t insn_step(trace_t *t, size_t cur, int dir)
{
    if (dir > 0)
        return cur + 1 < t->n_steps ? cur + 1 : cur;
    return cur > 0 ? cur - 1 : 0;
}

/* 'f': jump forward to the next call/ret instruction */
static size_t fn_boundary(trace_t *t, size_t cur)
{
    for (size_t i = cur + 1; i < t->n_steps; i++) {
        int32_t idx = t->steps[i].insn_idx;
        if (idx < 0)
            continue;
        const char *x = t->dlines[idx].text;
        if (strncmp(x, "call", 4) == 0 || strncmp(x, "ret", 3) == 0)
            return i;
    }
    return t->n_steps - 1;
}

/* 'g': numeric prompt on the status line */
static void goto_step(ui_t *u)
{
    char buf[32] = {0};
    int n = 0;
    for (;;) {
        move(LINES - 1, 0);
        clrtoeol();
        attron(A_REVERSE);
        printw(" goto step (Enter=jump, Esc=cancel): %s", buf);
        attroff(A_REVERSE);
        refresh();
        int ch = getch();
        if (ch == 27) /* ESC */
            return;
        if (ch == '\n' || ch == KEY_ENTER)
            break;
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && n > 0)
            buf[--n] = '\0';
        else if (isdigit(ch) && n < (int)sizeof(buf) - 1) {
            buf[n++] = (char)ch;
            buf[n] = '\0';
        }
    }
    if (n == 0)
        return;
    size_t v = (size_t)strtoull(buf, NULL, 10);
    if (v >= u->t->n_steps)
        v = u->t->n_steps - 1;
    u->cur = v;
}

/* ---------------- main loop ---------------- */

int tui_run(trace_t *t)
{
    setlocale(LC_ALL, "");
    if (!initscr())
        return -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_CUR,   COLOR_BLACK, COLOR_CYAN);
        init_pair(CP_CHG,   COLOR_RED,   -1);
        init_pair(CP_MARK,  COLOR_YELLOW, -1);
        init_pair(CP_TITLE, COLOR_CYAN,  -1);
        init_pair(CP_BAD,   COLOR_RED,   -1);
    }

    ui_t u = { .t = t, .cur = 0, .stack_desc = 1 };
    make_windows(&u);
    redraw(&u);

    for (;;) {
        int ch = getch();
        switch (ch) {
        case KEY_RIGHT: case 'n':
            u.cur = u.mode_src ? src_step(t, u.cur, +1)
                               : insn_step(t, u.cur, +1);
            break;
        case KEY_LEFT: case 'p':
            u.cur = u.mode_src ? src_step(t, u.cur, -1)
                               : insn_step(t, u.cur, -1);
            break;
        case 'm':
            u.mode_src = !u.mode_src;
            break;
        case '\t':
            u.mem_pane = (u.mem_pane + 1) % PANE_N;
            break;
        case KEY_UP:
            u.mem_scroll[u.mem_pane]--;
            break;
        case KEY_DOWN:
            u.mem_scroll[u.mem_pane]++;
            break;
        case 'd':
            u.stack_desc = !u.stack_desc; /* stack direction toggle */
            break;
        case 'g':
            goto_step(&u);
            break;
        case KEY_HOME:
            u.cur = 0;
            break;
        case KEY_END:
            u.cur = t->n_steps - 1;
            break;
        case 'o':
            u.show_out = !u.show_out;
            u.show_sys = 0;
            make_windows(&u);
            break;
        case 's':
            u.show_sys = !u.show_sys;
            u.show_out = 0;
            make_windows(&u);
            break;
        case 'f':
            u.cur = fn_boundary(t, u.cur);
            break;
        case KEY_RESIZE:
            make_windows(&u);
            break;
        case 'q':
            goto done;
        default:
            continue;
        }
        redraw(&u);
    }
done:
    destroy_windows(&u);
    endwin();
    return 0;
}
