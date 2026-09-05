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

/* the one pane -> name mapping (status bar short name, panel title) */
static const struct {
    const char *short_name, *title;
} PANE_INFO[PANE_N] = {
    [PANE_STACK]   = { "stack",   "stack" },
    [PANE_HEAP]    = { "heap",    "heap (brk+mmap)" },
    [PANE_GLOBALS] = { "globals", "globals (.got/.data/.bss)" },
    [PANE_RODATA]  = { "rodata",  "rodata (string literals, constants)" },
    [PANE_CODE]    = { "code",    "code (.text machine bytes)" },
};

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
    CP_VAR,       /* variable names/values (libdw) */
    CP_STR,       /* printable-string runs under hex rows */
};

/* at most one overlay (bottom half of the screen) is open at a time */
enum { OV_NONE = 0, OV_OUT, OV_SYS, OV_VARS };

/* one resolvable variable of the current step: local/param of the function
 * at RIP, or an image global — with its value snapshot-decoded */
typedef struct {
    const dvar_t *v;
    uint64_t      addr;
    char          val[48];
    int           has_val;
    int           changed;
} varview_t;
#define MAX_VARVIEW 64

typedef struct {
    trace_t *t;
    int      proc;                  /* current process (proc index) */
    size_t   cur;                   /* step index within that process */
    int      mode_src;              /* 0 = instruction step, 1 = source step */
    int      mem_pane;
    int      mem_scroll[PANE_N];    /* row offset per memory pane */
    int      stack_desc;            /* 1 = high addresses on top (CSAPP) */
    int      overlay;               /* OV_* */
    int      wide;                  /* 4x2 layout: all memory panes shown */
    WINDOW  *wsrc, *wasm, *wreg, *wmem, *wovl;
    WINDOW  *wmems[PANE_N];         /* wide mode: one window per section */
    /* variable views of the current step, built once per redraw */
    varview_t      vvs[MAX_VARVIEW];
    int            n_vvs;
    const dfunc_t *vv_func;
} ui_t;

/* ---------------- helpers ---------------- */

static const proc_t *cur_proc(ui_t *u) { return &u->t->procs[u->proc]; }
static const step_t *cur_step(ui_t *u)
{
    return &cur_proc(u)->steps[u->cur];
}
static const step_t *prev_step(ui_t *u)
{
    return &cur_proc(u)->steps[u->cur > 0 ? u->cur - 1 : 0];
}
static const image_t *cur_img(ui_t *u)
{
    return u->t->images[cur_step(u)->img];
}

/* ---------------- variable views (libdw) ---------------- */

static void fill_varview(ui_t *u, const dfunc_t *f, const dvar_t *v,
                         varview_t *vv)
{
    const trace_t *t = u->t;
    const step_t *s = cur_step(u), *p = prev_step(u);
    vv->v = v;
    vv->addr = dvar_addr(f, v, s->regs.rbp);
    vv->has_val = 0;
    vv->changed = 0;
    uint64_t raw, praw;
    if (step_read(t, s, vv->addr, v->size, &raw) == 0) {
        vv->has_val = 1;
        dvar_fmt_val(v, raw, vv->val, sizeof(vv->val));
        uint64_t paddr = dvar_addr(f, v, p->regs.rbp);
        vv->changed = (u->cur > 0 &&
                       (step_read(t, p, paddr, v->size, &praw) < 0 ||
                        praw != raw));
    } else {
        snprintf(vv->val, sizeof(vv->val), "?");
    }
}

/* rebuild u->vvs for the current step: locals/params of the function at
 * RIP first, then image globals — called once per redraw */
static void refresh_varviews(ui_t *u)
{
    const image_t *im = cur_img(u);
    const dfunc_t *f = img_func_at(im, cur_step(u)->regs.rip);
    int n = 0;
    if (f)
        for (int i = 0; i < f->n_vars && n < MAX_VARVIEW; i++)
            fill_varview(u, f, &f->vars[i], &u->vvs[n++]);
    for (int i = 0; i < im->n_gvars && n < MAX_VARVIEW; i++)
        fill_varview(u, f, &im->gvars[i], &u->vvs[n++]);
    u->n_vvs = n;
    u->vv_func = f;
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

/* pad the current-row highlight to the panel edge and drop the attr */
static void hl_row_end(WINDOW *w, int y, int width)
{
    for (int x = getcurx(w); x < width + 1; x++)
        mvwaddch(w, y, x, ' ');
    wattroff(w, COLOR_PAIR(CP_CUR) | A_BOLD);
}

/* ---------------- source panel ---------------- */

static void draw_src(ui_t *u)
{
    const image_t *im = cur_img(u);
    char title[600];
    snprintf(title, sizeof(title), "%s",
             im->src_file[0] ? im->src_file : "source");
    draw_frame(u->wsrc, title);

    int h = getmaxy(u->wsrc) - 2, w = getmaxx(u->wsrc) - 2;
    if (h <= 0)
        return;
    if (im->n_src == 0) {
        mvwprintw(u->wsrc, 1, 2, "(source not available)");
        wnoutrefresh(u->wsrc);
        return;
    }

    int cur_line = cur_step(u)->src_line; /* 1-based, -1 = none */
    int pos = (cur_line > 0 ? cur_line : 1) - 1;
    int first = center_first(pos, im->n_src, h);

    for (int i = 0; i < h && first + i < im->n_src; i++) {
        int lineno = first + i + 1;
        int is_cur = (lineno == cur_line);
        if (is_cur)
            wattron(u->wsrc, COLOR_PAIR(CP_CUR) | A_BOLD);
        mvwprintw(u->wsrc, 1 + i, 1, "%c%4d  %-.*s",
                  is_cur ? '>' : ' ', lineno,
                  w > 7 ? w - 7 : 0, im->src[first + i]);
        if (is_cur)
            hl_row_end(u->wsrc, 1 + i, w);
    }
    wnoutrefresh(u->wsrc);
}

/* ---------------- disassembly panel ---------------- */

static void draw_asm(ui_t *u)
{
    const image_t *im = cur_img(u);
    draw_frame(u->wasm, "disassembly");
    int h = getmaxy(u->wasm) - 2, w = getmaxx(u->wasm) - 2;
    if (h <= 0)
        return;

    int cur = cur_step(u)->insn_idx;
    int first = center_first(cur >= 0 ? cur : 0, (int)im->n_dlines, h);

    for (int i = 0; i < h && (size_t)(first + i) < im->n_dlines; i++) {
        const dline_t *d = &im->dlines[first + i];
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
        if (is_cur)
            hl_row_end(u->wasm, 1 + i, w);
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
        int changed = (u->cur > 0 && v != pv && i != CV_REG_RIP);
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
    const image_t *im = cur_img(u);
    *name = PANE_INFO[pane].title;
    switch (pane) {
    case PANE_STACK:
        *base = s->stack_base; *len = s->stack_len; *buf = s->stack;
        break;
    case PANE_RODATA: /* read-only: loaded once from the ELF, never changes */
        *base = im->rodata_rng.start;
        *len  = im->rodata_bytes ? im->rodata_rng.end - im->rodata_rng.start
                                 : 0;
        *buf  = im->rodata_bytes;
        break;
    case PANE_CODE:
        *base = im->text.start;
        *len  = im->text_bytes ? im->text.end - im->text.start : 0;
        *buf  = im->text_bytes;
        break;
    default:
        *base = im->globals_rng.start; *len = s->globals_len;
        *buf = s->globals;
        break;
    }
}

/* byte fetch for hex rows: static sections (sbuf) read the ELF-file
 * bytes, everything else the per-step snapshots */
static int hexrow_byte(ui_t *u, const step_t *s, uint64_t a, uint64_t base,
                       size_t len, const uint8_t *sbuf, uint8_t *out)
{
    if (a < base || a >= base + len)
        return -1;
    if (sbuf) {
        *out = sbuf[a - base];
        return 0;
    }
    return step_byte(u->t, s, a, out);
}

/* string detection for one hex row: a byte is part of a string when it
 * sits in a printable run of 4+ chars.  Scans 3 bytes past both row
 * edges so a string crossing rows keeps every piece.  Fills ch[8] (the
 * characters) and in_str[8]; returns nonzero if the row has any. */
static int hexrow_str(ui_t *u, uint64_t row_addr, uint64_t base, size_t len,
                      const uint8_t *sbuf, char ch[8], int in_str[8])
{
    const step_t *s = cur_step(u);
    int pr[14], run[14], any = 0;
    for (int i = 0; i < 14; i++) {
        uint8_t v;
        uint64_t a = row_addr + (uint64_t)i - 3;
        pr[i] = (row_addr + (uint64_t)i >= 3 &&
                 hexrow_byte(u, s, a, base, len, sbuf, &v) == 0 &&
                 v >= 0x20 && v < 0x7f);
        if (pr[i] && i >= 3 && i < 11)
            ch[i - 3] = (char)v;
        run[i] = pr[i] ? (i ? run[i - 1] + 1 : 1) : 0;
    }
    for (int i = 12; i >= 0; i--)
        if (pr[i] && run[i + 1] > run[i])
            run[i] = run[i + 1];
    for (int b = 0; b < 8; b++) {
        in_str[b] = pr[b + 3] && run[b + 3] >= 4;
        any |= in_str[b];
    }
    return any;
}

/* one hex row of 8 bytes with per-byte change highlighting; sbuf != NULL
 * means a static section (bytes from the ELF file, never change) instead
 * of per-step snapshots.  When the row holds a string (see hexrow_str)
 * and a second line fits, its characters are drawn on the next line,
 * column-aligned under their bytes.  Returns the number of lines used
 * (1 or 2), leaving the cursor at the end of the hex line so callers
 * can append markers/annotations to it. */
static int draw_hex_row(ui_t *u, WINDOW *w, int y, uint64_t row_addr,
                        uint64_t base, size_t len, const uint8_t *sbuf)
{
    const step_t *s = cur_step(u), *p = prev_step(u);
    mvwprintw(w, y, 1, "%012llx ", (unsigned long long)row_addr);
    for (int b = 0; b < 8; b++) {
        uint64_t a = row_addr + (uint64_t)b;
        uint8_t v;
        int have = (hexrow_byte(u, s, a, base, len, sbuf, &v) == 0);
        int changed = 0;
        if (have && !sbuf) {
            uint8_t pv;
            changed = (u->cur > 0 &&
                       (step_byte(u->t, p, a, &pv) < 0 || pv != v));
        }
        if (!have) {
            wprintw(w, " ..");
            continue;
        }
        if (changed)
            wattron(w, COLOR_PAIR(CP_CHG) | A_BOLD);
        wprintw(w, " %02x", v);
        if (changed)
            wattroff(w, COLOR_PAIR(CP_CHG) | A_BOLD);
    }

    char ch[8];
    int in_str[8];
    if (y + 1 > getmaxy(w) - 2 ||
        !hexrow_str(u, row_addr, base, len, sbuf, ch, in_str))
        return 1;
    int hex_end = getcurx(w);
    wattron(w, COLOR_PAIR(CP_STR) | A_BOLD);
    for (int b = 0; b < 8; b++)
        if (in_str[b]) /* under the byte's hex digits: " %02x" columns */
            mvwaddch(w, y + 1, 14 + 3 * b + 1,
                     (chtype)(unsigned char)ch[b]);
    wattroff(w, COLOR_PAIR(CP_STR) | A_BOLD);
    wmove(w, y, hex_end);
    return 2;
}

/* heap pane: [heap] plus tracked anonymous mmap regions, stacked */
static void draw_mem_heap(ui_t *u, WINDOW *w, int focus)
{
    const step_t *s = cur_step(u);
    char title[80];
    snprintf(title, sizeof(title), "%s%s", PANE_INFO[PANE_HEAP].title,
             u->wide ? "" : "  (Tab: next pane)");
    draw_frame_f(w, title, focus);
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

    int line = 1;
    for (int vrow = first; line <= h && vrow < total; vrow++) {
        int r = 0;
        while (r + 1 < s->n_heapr && reg_first[r + 1] <= vrow)
            r++;
        const heapreg_t *hr = &s->heapr[r];
        int inner = vrow - reg_first[r];
        if (inner == 0) {
            wattron(w, COLOR_PAIR(CP_MARK) | A_BOLD);
            mvwprintw(w, line, 1, "== %s 0x%llx (%zu bytes) ==",
                      r == 0 ? "region" : "mmap",
                      (unsigned long long)hr->base, hr->len);
            wattroff(w, COLOR_PAIR(CP_MARK) | A_BOLD);
            line++;
        } else {
            uint64_t row_addr = hr->base + (uint64_t)(inner - 1) * 8;
            line += draw_hex_row(u, w, line, row_addr, hr->base, hr->len,
                                 NULL);
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

    /* variable annotations for stack/globals rows (libdw); u->vvs is
     * refreshed once per redraw */
    int n_vv = (pane == PANE_STACK || pane == PANE_GLOBALS) ? u->n_vvs : 0;

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

    int line = 1;
    for (int disp = first; line <= h && disp < nrows; disp++) {
        int rowi = descending ? nrows - 1 - disp : disp;
        uint64_t row_addr = lo + (uint64_t)rowi * 8;

        const uint8_t *sbuf =
            (pane == PANE_CODE)   ? cur_img(u)->text_bytes :
            (pane == PANE_RODATA) ? cur_img(u)->rodata_bytes : NULL;
        line += draw_hex_row(u, w, line, row_addr, base, len, sbuf);

        /* markers/annotations append to the hex line — draw_hex_row
         * leaves the cursor there even when it drew a string line */
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

        /* inline variable annotations: names whose address is in this row */
        for (int k = 0; k < n_vv; k++) {
            const varview_t *vv = &u->vvs[k];
            if (vv->addr < row_addr || vv->addr >= row_addr + 8)
                continue;
            if (pane == PANE_GLOBALS && !vv->v->is_global)
                continue;
            if (pane == PANE_STACK && vv->v->is_global)
                continue;
            wattron(w, COLOR_PAIR(CP_VAR) | A_BOLD);
            wprintw(w, " %s=", vv->v->name);
            wattroff(w, COLOR_PAIR(CP_VAR) | A_BOLD);
            int vattr = vv->changed ? (COLOR_PAIR(CP_CHG) | A_BOLD)
                                    : COLOR_PAIR(CP_VAR);
            wattron(w, vattr);
            wprintw(w, "%s", vv->val);
            wattroff(w, vattr);
        }
    }
    wnoutrefresh(w);
}

/* ---------------- output overlay ---------------- */

static size_t output_len_at(const trace_t *t, uint64_t gseq)
{
    size_t n = 0;
    for (size_t i = 0; i < t->n_chunks; i++)
        if (t->chunks[i].gseq <= gseq)
            n = t->chunks[i].off + t->chunks[i].len;
    return n;
}

static void draw_out(ui_t *u)
{
    WINDOW *w = u->wovl;
    draw_frame(w, "program output (o: close)");
    int h = getmaxy(w) - 2, width = getmaxx(w) - 2;
    if (h <= 0)
        return;

    size_t len = output_len_at(u->t, cur_step(u)->gseq);
    const char *out = u->t->prog_output;
    if (len == 0) {
        mvwprintw(w, 1, 2, "(no output up to this step)");
        wnoutrefresh(w);
        return;
    }

    /* find the start of the last h lines by scanning backward: skip a
     * trailing newline, then step back one newline per displayed line */
    size_t start = len;
    int want = h + (out[len - 1] == '\n' ? 1 : 0);
    while (start > 0 && want > 0) {
        start--;
        if (out[start] == '\n')
            want--;
    }
    if (start > 0)
        start++; /* stopped ON a newline: the tail begins after it */

    for (int row = 0; row < h && start < len; row++) {
        const char *ls = out + start;
        const char *e = memchr(ls, '\n', len - start);
        size_t ll = e ? (size_t)(e - ls) : len - start;
        mvwprintw(w, 1 + row, 1, "%.*s",
                  (int)(ll > (size_t)width ? (size_t)width : ll), ls);
        start += ll + 1;
    }
    wnoutrefresh(w);
}

/* ---------------- syscall log overlay ---------------- */

static void draw_sys(ui_t *u)
{
    WINDOW *w = u->wovl;
    draw_frame(w, "syscalls up to this step (s: close)");
    int h = getmaxy(w) - 2, width = getmaxx(w) - 2;
    if (h <= 0)
        return;

    const trace_t *t = u->t;
    /* single backward pass: collect the last h matching events (this
     * process, step <= cur), then print them oldest-first */
    size_t idx[64]; /* h is bounded by the overlay height */
    int n = 0;
    int max = h < (int)(sizeof(idx) / sizeof(idx[0]))
                  ? h : (int)(sizeof(idx) / sizeof(idx[0]));
    for (size_t i = t->n_scs; i-- > 0 && n < max; ) {
        if (t->scs[i].proc == u->proc && t->scs[i].step <= u->cur)
            idx[n++] = i;
    }
    if (n == 0)
        mvwprintw(w, 1, 2, "(no syscalls up to this step)");

    for (int row = 0; row < n; row++) {
        const scevent_t *e = &t->scs[idx[n - 1 - row]];
        char scbuf[128], line[160];
        cv_format_syscall(e, scbuf, sizeof(scbuf));
        snprintf(line, sizeof(line), "step %5zu  %s", e->step, scbuf);
        int is_cur = (e->step == u->cur);
        if (is_cur)
            wattron(w, A_BOLD);
        mvwprintw(w, 1 + row, 1, "%-.*s", width, line);
        if (is_cur)
            wattroff(w, A_BOLD);
    }
    wnoutrefresh(w);
}

/* ---------------- variables overlay ---------------- */

static void draw_vars(ui_t *u)
{
    WINDOW *w = u->wovl;
    draw_frame(w, "variables (v: close)");
    int h = getmaxy(w) - 2, width = getmaxx(w) - 2;
    if (h <= 0)
        return;

    const dfunc_t *f = u->vv_func;
    int row = 0;
    if (f) {
        wattron(w, A_BOLD);
        mvwprintw(w, 1 + row++, 1, "fn %s  [0x%llx-0x%llx]",
                  f->name, (unsigned long long)f->lo,
                  (unsigned long long)f->hi);
        wattroff(w, A_BOLD);
    } else {
        mvwprintw(w, 1 + row++, 1, "(no function info at this address)");
    }

    int shown_globals_hdr = 0;
    for (int i = 0; i < u->n_vvs && row < h; i++) {
        const varview_t *vv = &u->vvs[i];
        const dvar_t *v = vv->v;
        if (v->is_global && !shown_globals_hdr) {
            wattron(w, A_BOLD);
            mvwprintw(w, 1 + row++, 1, "globals:");
            wattroff(w, A_BOLD);
            shown_globals_hdr = 1;
            if (row >= h)
                break;
        }

        char loc[40];
        if (v->is_global) {
            snprintf(loc, sizeof(loc), "0x%llx",
                     (unsigned long long)v->addr);
        } else if (vv->has_val) {
            snprintf(loc, sizeof(loc), "rbp%+lld  (0x%llx)",
                     (long long)dvar_rbp_off(f, v),
                     (unsigned long long)vv->addr);
        } else {
            /* prologue: RBP not set up yet, only the static offset is
             * meaningful */
            snprintf(loc, sizeof(loc), "rbp%+lld",
                     (long long)dvar_rbp_off(f, v));
        }

        int y = 1 + row++;
        wattron(w, COLOR_PAIR(CP_VAR) | A_BOLD);
        mvwprintw(w, y, 1, "  %-14.14s", v->name);
        wattroff(w, COLOR_PAIR(CP_VAR) | A_BOLD);
        wattron(w, A_DIM);
        wprintw(w, " : %-12.12s%s", v->type,
                v->is_param ? " param " : "       ");
        wattroff(w, A_DIM);
        wprintw(w, "= ");
        int vattr = vv->changed ? (COLOR_PAIR(CP_CHG) | A_BOLD)
                                : COLOR_PAIR(CP_VAR);
        wattron(w, vattr);
        wprintw(w, "%-22.22s",
                vv->has_val ? vv->val : (v->size ? "?" : "<agg>"));
        wattroff(w, vattr);
        wattron(w, A_DIM);
        wprintw(w, " @ %-.*s", width - getcurx(w) - 1, loc);
        wattroff(w, A_DIM);
    }
    if (u->n_vvs == 0 && row < h)
        mvwprintw(w, 1 + row, 1,
                  "(no variables — was the target built with -g?)");
    wnoutrefresh(w);
}

/* ---------------- status bar ---------------- */

static void draw_status(ui_t *u)
{
    trace_t *t = u->t;
    move(LINES - 1, 0);
    clrtoeol();
    attron(A_REVERSE);

    const proc_t *p = cur_proc(u);
    char skipbuf[32] = "";
    if (p->steps[u->cur].skipped)
        snprintf(skipbuf, sizeof(skipbuf), " (+%u libc)",
                 p->steps[u->cur].skipped);
    char procbuf[80] = "";
    if (t->n_procs > 1)
        snprintf(procbuf, sizeof(procbuf), " | proc %d/%d pid %d%s",
                 u->proc + 1, t->n_procs, p->pid,
                 p->parent >= 0 ? " (child)" : "");
    char left[320];
    snprintf(left, sizeof(left),
             " step %zu/%zu%s%s | mode: %s | mem: %s%s",
             u->cur, p->n_steps - 1, skipbuf, procbuf,
             u->mode_src ? "SRC " : "INSN",
             PANE_INFO[u->mem_pane].short_name,
             t->truncated ? " | TRUNCATED" : "");
    mvprintw(LINES - 1, 0, "%-.*s", COLS, left);

    if (p->execed && !p->followed) {
        attron(COLOR_PAIR(CP_BAD) | A_BOLD);
        printw(" | exec'd: not followed past exec");
        attroff(COLOR_PAIR(CP_BAD) | A_BOLD);
        attron(A_REVERSE);
    }
    if (p->death_signal) {
        attron(COLOR_PAIR(CP_BAD) | A_BOLD);
        printw(" | killed: signal %d", p->death_signal);
        attroff(COLOR_PAIR(CP_BAD) | A_BOLD);
        attron(A_REVERSE);
    } else {
        printw(" | exit %d", p->exit_code);
    }

    const char *help =
        " | </> step  P proc  m mode  Tab mem  g goto  f fn  v vars  "
        "o out  s sys  q quit";
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
    if (u->wovl) delwin(u->wovl);
    u->wsrc = u->wasm = u->wreg = u->wmem = u->wovl = NULL;
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
    if (u->overlay != OV_NONE) {
        int oh = usable / 2;
        u->wovl = newwin(oh, COLS, usable - oh, 0);
    }
}

static void redraw(ui_t *u)
{
    refresh_varviews(u); /* shared by mem-pane annotations and 'v' */
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
    switch (u->overlay) {
    case OV_OUT:  draw_out(u);  break;
    case OV_SYS:  draw_sys(u);  break;
    case OV_VARS: draw_vars(u); break;
    }
    draw_status(u);
    doupdate();
}

/* ---------------- stepping ---------------- */

/* source-line mode: first index (in direction dir) whose valid src_line
 * differs from the current line (spec 6.3 "스텝 모드 구현") */
static size_t src_step(const proc_t *p, size_t cur, int dir)
{
    int32_t line = p->steps[cur].src_line;
    long i = (long)cur + dir;
    while (i >= 0 && i < (long)p->n_steps) {
        int32_t l = p->steps[i].src_line;
        if (l != -1 && l != line)
            return (size_t)i;
        i += dir;
    }
    return dir > 0 ? p->n_steps - 1 : 0;
}

static size_t insn_step(const proc_t *p, size_t cur, int dir)
{
    if (dir > 0)
        return cur + 1 < p->n_steps ? cur + 1 : cur;
    return cur > 0 ? cur - 1 : 0;
}

/* 'f': jump forward to the next call/ret instruction */
static size_t fn_boundary(const trace_t *t, const proc_t *p, size_t cur)
{
    for (size_t i = cur + 1; i < p->n_steps; i++) {
        int32_t idx = p->steps[i].insn_idx;
        if (idx < 0)
            continue;
        const char *x = t->images[p->steps[i].img]->dlines[idx].text;
        if (strncmp(x, "call", 4) == 0 || strncmp(x, "ret", 3) == 0)
            return i;
    }
    return p->n_steps - 1;
}

/* 'P': next process with steps, landing on the step whose global sequence
 * number is closest to the current one (time-synchronized switch) */
static void switch_proc(ui_t *u)
{
    trace_t *t = u->t;
    if (t->n_procs < 2)
        return;
    uint64_t g = cur_step(u)->gseq;
    int np = u->proc;
    for (int k = 0; k < t->n_procs; k++) {
        np = (np + 1) % t->n_procs;
        if (np != u->proc && t->procs[np].n_steps > 0)
            break;
    }
    proc_t *p = &t->procs[np];
    if (np == u->proc || p->n_steps == 0)
        return;

    size_t lo = 0, hi = p->n_steps;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->steps[mid].gseq < g)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= p->n_steps)
        lo = p->n_steps - 1;
    else if (lo > 0 && g - p->steps[lo - 1].gseq < p->steps[lo].gseq - g)
        lo--;
    u->proc = np;
    u->cur = lo;
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
    if (v >= cur_proc(u)->n_steps)
        v = cur_proc(u)->n_steps - 1;
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
        init_pair(CP_VAR,   COLOR_GREEN, -1);
        init_pair(CP_STR,   COLOR_MAGENTA, -1);
    }

    ui_t u = { .t = t, .cur = 0, .stack_desc = 1 };
    for (int pi = 0; pi < t->n_procs; pi++) { /* first proc with steps */
        if (t->procs[pi].n_steps > 0) {
            u.proc = pi;
            break;
        }
    }
    make_windows(&u);
    redraw(&u);

    for (;;) {
        int ch = getch();
        switch (ch) {
        case KEY_RIGHT: case 'n':
            u.cur = u.mode_src ? src_step(cur_proc(&u), u.cur, +1)
                               : insn_step(cur_proc(&u), u.cur, +1);
            break;
        case KEY_LEFT: case 'p':
            u.cur = u.mode_src ? src_step(cur_proc(&u), u.cur, -1)
                               : insn_step(cur_proc(&u), u.cur, -1);
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
            u.cur = cur_proc(&u)->n_steps - 1;
            break;
        case 'P':
            switch_proc(&u);
            break;
        case 'o':
            u.overlay = (u.overlay == OV_OUT) ? OV_NONE : OV_OUT;
            make_windows(&u);
            break;
        case 's':
            u.overlay = (u.overlay == OV_SYS) ? OV_NONE : OV_SYS;
            make_windows(&u);
            break;
        case 'v':
            u.overlay = (u.overlay == OV_VARS) ? OV_NONE : OV_VARS;
            make_windows(&u);
            break;
        case 'f':
            u.cur = fn_boundary(t, cur_proc(&u), u.cur);
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
