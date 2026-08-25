/* dwarfvars.c - variable names/types/locations from DWARF via libdw
 * (spec 6.4, Phase 2 extension)
 *
 * At -O0 the picture is simple and we only handle that picture:
 *  - locals/params live at DW_OP_fbreg <sleb-offset> from the frame base
 *  - the frame base is DW_OP_call_frame_cfa (== RBP+16 with -fno-omit-
 *    frame-pointer once the prologue ran) or DW_OP_breg6 (RBP-relative)
 *  - globals live at DW_OP_addr <address>
 * Anything fancier (registers, location lists) is silently skipped —
 * that is optimized-code territory, out of scope by the build contract.
 *
 * Everything here soft-fails: without usable DWARF the image just has
 * no variable info and the TUI shows none.
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- type resolution ---------------- */

/* follow DW_AT_type through typedef/const/volatile; fill name/size/enc.
 * Pointer depth is rendered as trailing '*'s; aggregates keep size 0
 * so the UI shows their address instead of a bogus scalar value. */
static void resolve_type(Dwarf_Die *var, dvar_t *out)
{
    Dwarf_Attribute a;
    Dwarf_Die t_mem, *t = var;
    int ptr = 0;
    const char *name = NULL;

    for (int depth = 0; depth < 16; depth++) {
        if (!dwarf_attr_integrate(t, DW_AT_type, &a))
            break;
        if (!dwarf_formref_die(&a, &t_mem))
            break;
        t = &t_mem;
        int tag = dwarf_tag(t);
        if (tag == DW_TAG_pointer_type) {
            ptr++;
            continue;
        }
        if (tag == DW_TAG_typedef && !name)
            name = dwarf_diename(t); /* keep the typedef's name */
        if (tag == DW_TAG_typedef || tag == DW_TAG_const_type ||
            tag == DW_TAG_volatile_type)
            continue;

        /* a concrete type */
        if (!name)
            name = dwarf_diename(t);
        if (tag == DW_TAG_base_type || tag == DW_TAG_enumeration_type) {
            Dwarf_Word w;
            if (dwarf_attr_integrate(t, DW_AT_byte_size, &a) &&
                dwarf_formudata(&a, &w) == 0)
                out->size = (uint32_t)w;
            if (dwarf_attr_integrate(t, DW_AT_encoding, &a) &&
                dwarf_formudata(&a, &w) == 0)
                out->enc = (int)w;
            if (tag == DW_TAG_enumeration_type) {
                out->enc = DW_ATE_signed;
                if (!name)
                    name = "enum";
            }
        } else if (tag == DW_TAG_structure_type) {
            if (!name)
                name = "struct";
        } else if (tag == DW_TAG_union_type) {
            if (!name)
                name = "union";
        } else if (tag == DW_TAG_array_type) {
            if (!name)
                name = "array";
        }
        break;
    }

    if (ptr) {
        out->is_ptr = 1;
        out->size = 8;
        out->enc = 0;
    }
    snprintf(out->type, sizeof(out->type), "%s%.*s",
             name ? name : (ptr ? "void" : "?"),
             ptr > 8 ? 8 : ptr, "********");
}

/* ---------------- location decoding ---------------- */

/* returns 0 and fills out on a location we understand */
static int decode_location(Dwarf_Die *die, dvar_t *out)
{
    Dwarf_Attribute a;
    Dwarf_Op *ops;
    size_t nops;
    if (!dwarf_attr_integrate(die, DW_AT_location, &a))
        return -1;
    if (dwarf_getlocation(&a, &ops, &nops) != 0 || nops != 1)
        return -1;
    if (ops[0].atom == DW_OP_fbreg) {
        out->is_global = 0;
        out->off = (int64_t)ops[0].number;
        return 0;
    }
    if (ops[0].atom == DW_OP_addr) {
        out->is_global = 1;
        out->addr = (uint64_t)ops[0].number;
        return 0;
    }
    return -1;
}

static int add_var(dvar_t **arr, int *n, Dwarf_Die *die, int is_param)
{
    const char *name = dwarf_diename(die);
    if (!name || !name[0])
        return 0;

    dvar_t v;
    memset(&v, 0, sizeof(v));
    if (decode_location(die, &v) < 0)
        return 0;
    snprintf(v.name, sizeof(v.name), "%s", name);
    v.is_param = is_param;
    resolve_type(die, &v);

    dvar_t *na = realloc(*arr, ((size_t)*n + 1) * sizeof(dvar_t));
    if (!na)
        return -1;
    *arr = na;
    na[(*n)++] = v;
    return 0;
}

/* ---------------- function walking ---------------- */

/* collect params/locals of a subprogram, recursing into lexical blocks */
static void collect_func_vars(Dwarf_Die *scope, dfunc_t *f)
{
    Dwarf_Die child;
    if (dwarf_child(scope, &child) != 0)
        return;
    do {
        int tag = dwarf_tag(&child);
        if (tag == DW_TAG_formal_parameter)
            add_var(&f->vars, &f->n_vars, &child, 1);
        else if (tag == DW_TAG_variable)
            add_var(&f->vars, &f->n_vars, &child, 0);
        else if (tag == DW_TAG_lexical_block)
            collect_func_vars(&child, f);
    } while (dwarf_siblingof(&child, &child) == 0);
}

static void frame_base_of(Dwarf_Die *sp, dfunc_t *f)
{
    f->fb_cfa = 1; /* the gcc -O0 default; assume it if undecodable */
    f->fb_off = 0;

    Dwarf_Attribute a;
    Dwarf_Op *ops;
    size_t nops;
    if (!dwarf_attr_integrate(sp, DW_AT_frame_base, &a) ||
        dwarf_getlocation(&a, &ops, &nops) != 0 || nops != 1)
        return;
    if (ops[0].atom == DW_OP_call_frame_cfa) {
        f->fb_cfa = 1;
    } else if (ops[0].atom == DW_OP_breg6) { /* rbp + offset */
        f->fb_cfa = 0;
        f->fb_off = (int64_t)ops[0].number;
    } else if (ops[0].atom == DW_OP_reg6) {  /* rbp */
        f->fb_cfa = 0;
        f->fb_off = 0;
    }
}

static int func_range(Dwarf_Die *sp, uint64_t *lo, uint64_t *hi)
{
    Dwarf_Addr l, h;
    if (dwarf_lowpc(sp, &l) != 0)
        return -1;
    if (dwarf_highpc(sp, &h) != 0) {
        /* DW_AT_high_pc as a constant offset from lowpc */
        Dwarf_Attribute a;
        Dwarf_Word w;
        if (!dwarf_attr_integrate(sp, DW_AT_high_pc, &a) ||
            dwarf_formudata(&a, &w) != 0)
            return -1;
        h = l + w;
    }
    if (h <= l)
        return -1;
    *lo = l;
    *hi = h;
    return 0;
}

static int func_cmp(const void *x, const void *y)
{
    const dfunc_t *a = x, *b = y;
    if (a->lo < b->lo) return -1;
    if (a->lo > b->lo) return 1;
    return 0;
}

/* ---------------- entry points ---------------- */

int dw_load_vars(image_t *im, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    Dwarf *dw = dwarf_begin(fd, DWARF_C_READ);
    if (!dw) {
        close(fd);
        return -1;
    }

    Dwarf_Off off = 0, next;
    size_t hsize;
    while (dwarf_nextcu(dw, off, &next, &hsize, NULL, NULL, NULL) == 0) {
        Dwarf_Die cu;
        if (dwarf_offdie(dw, off + hsize, &cu)) {
            Dwarf_Die child;
            if (dwarf_child(&cu, &child) == 0) {
                do {
                    int tag = dwarf_tag(&child);
                    if (tag == DW_TAG_subprogram) {
                        dfunc_t f;
                        memset(&f, 0, sizeof(f));
                        if (func_range(&child, &f.lo, &f.hi) != 0)
                            continue;
                        const char *nm = dwarf_diename(&child);
                        snprintf(f.name, sizeof(f.name), "%s",
                                 nm ? nm : "?");
                        frame_base_of(&child, &f);
                        collect_func_vars(&child, &f);

                        dfunc_t *nf = realloc(im->funcs,
                                              ((size_t)im->n_funcs + 1)
                                                  * sizeof(dfunc_t));
                        if (!nf) {
                            free(f.vars);
                            continue;
                        }
                        im->funcs = nf;
                        im->funcs[im->n_funcs++] = f;
                    } else if (tag == DW_TAG_variable) {
                        add_var(&im->gvars, &im->n_gvars, &child, 0);
                    }
                } while (dwarf_siblingof(&child, &child) == 0);
            }
        }
        off = next;
    }
    dwarf_end(dw);
    close(fd);

    if (im->n_funcs > 1)
        qsort(im->funcs, (size_t)im->n_funcs, sizeof(dfunc_t), func_cmp);

    /* keep only globals inside the image (drops crt-internal ones and
     * anything without a real address) */
    int w = 0;
    for (int i = 0; i < im->n_gvars; i++) {
        const dvar_t *v = &im->gvars[i];
        if (v->is_global &&
            v->addr >= im->globals_rng.start &&
            v->addr <  im->globals_rng.end)
            im->gvars[w++] = *v;
    }
    im->n_gvars = w;
    return 0;
}

const dfunc_t *img_func_at(const image_t *im, uint64_t rip)
{
    int lo = 0, hi = im->n_funcs;
    while (lo < hi) { /* greatest lo <= rip */
        int mid = lo + (hi - lo) / 2;
        if (im->funcs[mid].lo <= rip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const dfunc_t *f = &im->funcs[lo - 1];
    return (rip < f->hi) ? f : NULL;
}

/* static offset from RBP; frame-base convention lives only here */
int64_t dvar_rbp_off(const dfunc_t *f, const dvar_t *v)
{
    if (!f)
        return v->off;
    /* CFA = RBP + 16 once "push %rbp; mov %rsp,%rbp" has run */
    return (f->fb_cfa ? 16 : f->fb_off) + v->off;
}

uint64_t dvar_addr(const dfunc_t *f, const dvar_t *v, uint64_t rbp)
{
    if (v->is_global)
        return v->addr;
    return rbp + (uint64_t)dvar_rbp_off(f, v);
}

void dvar_fmt_val(const dvar_t *v, uint64_t raw, char *buf, size_t n)
{
    if (v->is_ptr) {
        snprintf(buf, n, "0x%llx", (unsigned long long)raw);
        return;
    }
    switch (v->enc) {
    case DW_ATE_float:
        if (v->size == 4) {
            float f;
            uint32_t r32 = (uint32_t)raw;
            memcpy(&f, &r32, 4);
            snprintf(buf, n, "%g", (double)f);
        } else {
            double d;
            memcpy(&d, &raw, 8);
            snprintf(buf, n, "%g", d);
        }
        return;
    case DW_ATE_signed: case DW_ATE_signed_char: {
        int64_t sv = (int64_t)raw;
        if (v->size < 8) { /* sign-extend */
            uint64_t m = 1ull << (8 * v->size - 1);
            sv = (int64_t)((raw ^ m) - m);
        }
        if (v->enc == DW_ATE_signed_char && sv >= 0x20 && sv < 0x7f)
            snprintf(buf, n, "'%c' (%lld)", (char)sv, (long long)sv);
        else if (sv >= 4096 || sv <= -4096)
            snprintf(buf, n, "%lld (0x%llx)", (long long)sv,
                     (unsigned long long)raw);
        else
            snprintf(buf, n, "%lld", (long long)sv);
        return;
    }
    case DW_ATE_boolean:
        snprintf(buf, n, "%s", raw ? "true" : "false");
        return;
    case DW_ATE_unsigned_char:
        if (raw >= 0x20 && raw < 0x7f) {
            snprintf(buf, n, "'%c' (%llu)", (char)raw,
                     (unsigned long long)raw);
            return;
        }
        /* fall through */
    case DW_ATE_unsigned:
        if (raw >= 4096)
            snprintf(buf, n, "%llu (0x%llx)", (unsigned long long)raw,
                     (unsigned long long)raw);
        else
            snprintf(buf, n, "%llu", (unsigned long long)raw);
        return;
    default:
        snprintf(buf, n, "0x%llx", (unsigned long long)raw);
        return;
    }
}
