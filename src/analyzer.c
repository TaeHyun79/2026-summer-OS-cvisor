/* analyzer.c - static analysis of one ELF binary ("image"): objdump parsing
 * + ELF section ranges + source loading.
 *
 * Spec 6.1: disassembly and line table come from objdump output (robust
 * token-based parsing, not fixed-column sscanf); section ranges come from
 * parsing the ELF headers directly (Elf64_Ehdr/Elf64_Shdr).
 *
 * With follow-exec, an image is analyzed per binary: images[0] is the
 * traced program, further images appear when a followed process execs.
 */
#define _GNU_SOURCE
#include "cvisor.h"

#include <ctype.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* exec re-analysis failures (e.g. a PIE /bin/ls) are expected — quiet mode
 * suppresses the diagnostics that would otherwise spam the recording */
static int g_quiet;
#define A_ERR(...) do { if (!g_quiet) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------------- ELF section ranges ---------------- */

static int elf_analyze(image_t *im, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        A_ERR("cvisor: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        A_ERR("cvisor: %s: not an ELF file\n", path);
        close(fd);
        return -1;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        A_ERR("cvisor: mmap %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    const Elf64_Ehdr *eh = map;
    int rc = -1;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64) {
        A_ERR("cvisor: %s: not a 64-bit ELF binary\n", path);
        goto out;
    }
    if (eh->e_machine != EM_X86_64) {
        A_ERR("cvisor: %s: not an x86-64 binary\n", path);
        goto out;
    }
    if (eh->e_type == ET_DYN) {
        A_ERR("cvisor: %s is a PIE binary. Rebuild with:\n"
              "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n"
              "(PIE support is a Phase 3 item; objdump vs runtime addresses "
              "must match)\n", path);
        goto out;
    }
    if (eh->e_type != ET_EXEC) {
        A_ERR("cvisor: %s: not an executable (e_type=%d)\n", path, eh->e_type);
        goto out;
    }
    if (eh->e_shoff == 0 || eh->e_shstrndx == SHN_UNDEF) {
        A_ERR("cvisor: %s: no section headers (stripped?)\n", path);
        goto out;
    }

    const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)map + eh->e_shoff);
    const char *shstr = (const char *)map + sh[eh->e_shstrndx].sh_offset;

    /* runtime-mutable writable sections merged into the globals range:
     * .got/.got.plt sit right before .data, and watching a GOT entry flip
     * on the first printf call is lazy binding made visible */
    static const char *const WR_SECS[] = { ".got", ".got.plt", ".data",
                                           ".bss" };
    range_t wr = {0, 0};
    uint64_t text_off = 0, ro_off = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        const char *name = shstr + sh[i].sh_name;
        if (strcmp(name, ".text") == 0) {
            im->text.start = sh[i].sh_addr;
            im->text.end   = sh[i].sh_addr + sh[i].sh_size;
            text_off       = sh[i].sh_offset;
        } else if (strcmp(name, ".rodata") == 0) {
            im->rodata_rng.start = sh[i].sh_addr;
            im->rodata_rng.end   = sh[i].sh_addr + sh[i].sh_size;
            ro_off               = sh[i].sh_offset;
        } else {
            for (size_t k = 0; k < sizeof(WR_SECS) / sizeof(WR_SECS[0]); k++) {
                if (strcmp(name, WR_SECS[k]) != 0)
                    continue;
                if (wr.start == 0 || sh[i].sh_addr < wr.start)
                    wr.start = sh[i].sh_addr;
                if (sh[i].sh_addr + sh[i].sh_size > wr.end)
                    wr.end = sh[i].sh_addr + sh[i].sh_size;
            }
        }
    }
    if (im->text.start == 0) {
        A_ERR("cvisor: %s: no .text section\n", path);
        goto out;
    }

    im->globals_rng = wr;
    if (im->globals_rng.end - im->globals_rng.start > CV_GLOBALS_MAX)
        im->globals_rng.end = im->globals_rng.start + CV_GLOBALS_MAX;

    /* read-only sections: load the bytes once, straight from the file */
    size_t tlen = im->text.end - im->text.start;
    if (tlen > CV_STATIC_SEC_MAX) {
        tlen = CV_STATIC_SEC_MAX;
        im->text.end = im->text.start + tlen;
    }
    im->text_bytes = malloc(tlen);
    if (im->text_bytes)
        memcpy(im->text_bytes, (const char *)map + text_off, tlen);

    if (im->rodata_rng.end > im->rodata_rng.start) {
        size_t rlen = im->rodata_rng.end - im->rodata_rng.start;
        if (rlen > CV_STATIC_SEC_MAX) {
            rlen = CV_STATIC_SEC_MAX;
            im->rodata_rng.end = im->rodata_rng.start + rlen;
        }
        im->rodata_bytes = malloc(rlen);
        if (im->rodata_bytes)
            memcpy(im->rodata_bytes, (const char *)map + ro_off, rlen);
    }

    im->entry = eh->e_entry;
    rc = 0;
out:
    munmap(map, (size_t)st.st_size);
    return rc;
}

/* ---------------- objdump -d parsing ---------------- */

static char *xstrdup_trim(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *d = strdup(s);
    if (d) {
        size_t n = strlen(d);
        while (n > 0 && (d[n-1] == '\n' || d[n-1] == ' ' || d[n-1] == '\t'))
            d[--n] = '\0';
    }
    return d;
}

static int add_dline(image_t *im, uint64_t addr, int is_label, const char *text)
{
    if (im->n_dlines % 1024 == 0) {
        dline_t *nd = realloc(im->dlines,
                              (im->n_dlines + 1024) * sizeof(dline_t));
        if (!nd)
            return -1;
        im->dlines = nd;
    }
    dline_t *d = &im->dlines[im->n_dlines];
    d->addr = addr;
    d->is_label = is_label;
    d->text = xstrdup_trim(text);
    if (!d->text)
        return -1;
    im->n_dlines++;
    return 0;
}

static int parse_disasm(image_t *im, const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "objdump -d --no-show-raw-insn '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        A_ERR("cvisor: failed to run objdump -d\n");
        return -1;
    }

    char line[4096];
    size_t iref_cap = 0;
    while (fgets(line, sizeof(line), fp)) {
        const char *p = line;

        /* symbol line: "0000000000401136 <main>:" */
        if (isxdigit((unsigned char)*p)) {
            char *end;
            uint64_t addr = strtoull(p, &end, 16);
            const char *lt = strchr(end, '<');
            const char *gt = lt ? strchr(lt, '>') : NULL;
            if (lt && gt && gt[1] == ':') {
                char label[256];
                size_t n = (size_t)(gt + 1 - lt) + 1; /* len of "<name>:" */
                if (n > sizeof(label) - 1)
                    n = sizeof(label) - 1;
                memcpy(label, lt, n);
                label[n] = '\0';
                if (add_dline(im, addr, 1, label) < 0)
                    goto oom;
                if (strncmp(lt, "<main>:", 7) == 0)
                    im->main_addr = addr;
            }
            continue;
        }

        /* instruction line: "  401136:\tpush   %rbp" */
        while (*p == ' ' || *p == '\t')
            p++;
        if (!isxdigit((unsigned char)*p))
            continue;
        char *end;
        uint64_t addr = strtoull(p, &end, 16);
        if (*end != ':')
            continue;
        end++;
        while (*end == ' ' || *end == '\t')
            end++;
        if (*end == '\0' || *end == '\n')
            continue;
        if (strncmp(end, "...", 3) == 0) /* alignment ellipsis */
            continue;
        if (add_dline(im, addr, 0, end) < 0)
            goto oom;

        if (im->n_irefs == iref_cap) {
            iref_cap = iref_cap ? iref_cap * 2 : 1024;
            insn_ref_t *nr = realloc(im->irefs, iref_cap * sizeof(insn_ref_t));
            if (!nr)
                goto oom;
            im->irefs = nr;
        }
        im->irefs[im->n_irefs].addr = addr;
        im->irefs[im->n_irefs].dline_idx = (int32_t)(im->n_dlines - 1);
        im->n_irefs++;
    }
    pclose(fp);

    if (im->n_irefs == 0) {
        A_ERR("cvisor: objdump -d produced no instructions "
              "(is objdump installed?)\n");
        return -1;
    }
    return 0;
oom:
    pclose(fp);
    A_ERR("cvisor: out of memory parsing disassembly\n");
    return -1;
}

/* ---------------- objdump --dwarf=decodedline parsing ---------------- */

static int lmap_cmp(const void *a, const void *b)
{
    const lmap_t *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    return 0;
}

static int is_all_digits(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

#define CV_MAX_CUS 64

/*
 * Row formats vary across binutils versions; per spec, don't sscanf fixed
 * columns.  Tokenize and use: token starting with "0x" = address, the token
 * right before it = line number ("-" = end of sequence), first token = file.
 */
static int parse_decodedline(image_t *im, const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "objdump --dwarf=decodedline '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        A_ERR("cvisor: failed to run objdump --dwarf=decodedline\n");
        return -1;
    }

    /* multiple CUs can carry line info (e.g. musl's crt1.o); remember which
     * file each row came from so we can keep only the CU that owns main */
    char files[CV_MAX_CUS][256];
    int  n_files = 0;
    int *fids = NULL;

    char line[4096];
    size_t cap = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* "CU: ./target.c:" — remember compilation unit path */
        if (strncmp(line, "CU:", 3) == 0) {
            char *s = line + 3;
            while (*s == ' ')
                s++;
            size_t n = strlen(s);
            while (n && (s[n-1] == '\n' || s[n-1] == ':' || s[n-1] == ' '))
                s[--n] = '\0';
            if (n && im->src_file[0] == '\0')
                snprintf(im->src_file, sizeof(im->src_file), "%s", s);
            continue;
        }

        char *tok[64];
        int ntok = 0;
        char *save = NULL;
        for (char *p = strtok_r(line, " \t\n", &save);
             p && ntok < 64; p = strtok_r(NULL, " \t\n", &save))
            tok[ntok++] = p;
        if (ntok < 3)
            continue;

        int addr_i = -1;
        for (int i = 1; i < ntok; i++) {
            if (tok[i][0] == '0' && (tok[i][1] == 'x' || tok[i][1] == 'X')) {
                addr_i = i;
                break;
            }
        }
        if (addr_i < 2)
            continue;
        const char *line_tok = tok[addr_i - 1];
        int32_t lno;
        if (strcmp(line_tok, "-") == 0)
            lno = -1; /* end of sequence: addresses past here have no line */
        else if (is_all_digits(line_tok))
            lno = (int32_t)atoi(line_tok);
        else
            continue;

        uint64_t addr = strtoull(tok[addr_i], NULL, 16);
        if (addr == 0)
            continue;

        int fid = -1;
        for (int f = 0; f < n_files; f++) {
            if (strcmp(files[f], tok[0]) == 0) {
                fid = f;
                break;
            }
        }
        if (fid < 0 && n_files < CV_MAX_CUS) {
            snprintf(files[n_files], sizeof(files[0]), "%s", tok[0]);
            fid = n_files++;
        }

        if (im->n_lmap == cap) {
            cap = cap ? cap * 2 : 512;
            lmap_t *nl = realloc(im->lmap, cap * sizeof(lmap_t));
            int *nf = realloc(fids, cap * sizeof(int));
            if (!nl || !nf) {
                if (nl)
                    im->lmap = nl;
                if (nf)
                    fids = nf;
                free(fids);
                pclose(fp);
                A_ERR("cvisor: out of memory parsing line table\n");
                return -1;
            }
            im->lmap = nl;
            fids = nf;
        }
        im->lmap[im->n_lmap].addr = addr;
        im->lmap[im->n_lmap].line = lno;
        fids[im->n_lmap] = fid;
        im->n_lmap++;
    }
    pclose(fp);

    if (im->n_lmap == 0) {
        free(fids);
        A_ERR("cvisor: no DWARF line info in %s. Rebuild with:\n"
              "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n",
              path);
        return -1;
    }

    /* pick the CU that owns main (fallback: the one with the most rows);
     * rows from other CUs (crt startup etc.) become "no mapping" */
    int chosen = -1;
    if (im->main_addr) {
        uint64_t best = 0;
        for (size_t i = 0; i < im->n_lmap; i++) {
            if (im->lmap[i].addr <= im->main_addr && im->lmap[i].addr >= best &&
                im->lmap[i].line != -1) {
                best = im->lmap[i].addr;
                chosen = fids[i];
            }
        }
    }
    if (chosen < 0 && n_files > 0) {
        int counts[CV_MAX_CUS] = {0};
        for (size_t i = 0; i < im->n_lmap; i++)
            counts[fids[i]]++;
        chosen = 0;
        for (int f = 1; f < n_files; f++)
            if (counts[f] > counts[chosen])
                chosen = f;
    }
    if (chosen >= 0) {
        for (size_t i = 0; i < im->n_lmap; i++)
            if (fids[i] != chosen)
                im->lmap[i].line = -1;
        snprintf(im->src_file, sizeof(im->src_file), "%.255s", files[chosen]);
    }
    free(fids);

    qsort(im->lmap, im->n_lmap, sizeof(lmap_t), lmap_cmp);
    return 0;
}

/* ---------------- source loading ---------------- */

static FILE *open_source(const image_t *im, const char *target_path)
{
    FILE *f = fopen(im->src_file, "r");
    if (f)
        return f;

    /* try basename of src_file next to the target binary, then in cwd */
    char tmp[512], dirbuf[512];
    snprintf(tmp, sizeof(tmp), "%s", im->src_file);
    const char *base = basename(tmp);

    snprintf(dirbuf, sizeof(dirbuf), "%s", target_path);
    char joined[1200];
    snprintf(joined, sizeof(joined), "%s/%s", dirname(dirbuf), base);
    f = fopen(joined, "r");
    if (f)
        return f;
    return fopen(base, "r");
}

static int load_source(image_t *im, const char *target_path)
{
    if (im->src_file[0] == '\0')
        return -1;
    FILE *f = open_source(im, target_path);
    if (!f) {
        A_ERR("cvisor: warning: source file '%s' not found; "
              "source panel will be empty\n", im->src_file);
        return -1;
    }
    char line[4096];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r'))
            line[--n] = '\0';
        /* expand tabs so ncurses column math stays simple */
        char expanded[4096];
        size_t o = 0;
        for (size_t i = 0; i < n && o + 8 < sizeof(expanded); i++) {
            if (line[i] == '\t')
                do { expanded[o++] = ' '; } while (o % 4);
            else
                expanded[o++] = line[i];
        }
        expanded[o] = '\0';

        if (im->n_src == cap) {
            cap = cap ? cap * 2 : 256;
            char **ns = realloc(im->src, (size_t)cap * sizeof(char *));
            if (!ns) {
                fclose(f);
                return -1;
            }
            im->src = ns;
        }
        im->src[im->n_src] = strdup(expanded);
        if (!im->src[im->n_src]) {
            fclose(f);
            return -1;
        }
        im->n_src++;
    }
    fclose(f);
    return 0;
}

/* ---------------- entry points ---------------- */

image_t *image_analyze(const char *target_path, int quiet)
{
    g_quiet = quiet;
    image_t *im = calloc(1, sizeof(*im));
    if (!im)
        return NULL;
    snprintf(im->path, sizeof(im->path), "%s", target_path);

    if (elf_analyze(im, target_path) < 0 ||
        parse_disasm(im, target_path) < 0 ||
        parse_decodedline(im, target_path) < 0) {
        image_free(im);
        g_quiet = 0;
        return NULL;
    }
    load_source(im, target_path); /* soft failure: TUI degrades gracefully */
    g_quiet = 0;
    return im;
}

void image_free(image_t *im)
{
    if (!im)
        return;
    for (size_t i = 0; i < im->n_dlines; i++)
        free(im->dlines[i].text);
    free(im->dlines);
    free(im->irefs);
    free(im->lmap);
    for (int i = 0; i < im->n_src; i++)
        free(im->src[i]);
    free(im->src);
    free(im->text_bytes);
    free(im->rodata_bytes);
    free(im);
}

void image_dump(const image_t *im)
{
    printf("== ELF: %s ==\n", im->path);
    printf(".text        : 0x%llx - 0x%llx\n",
           (unsigned long long)im->text.start,
           (unsigned long long)im->text.end);
    printf(".got/.data/.bss: 0x%llx - 0x%llx (%llu bytes)\n",
           (unsigned long long)im->globals_rng.start,
           (unsigned long long)im->globals_rng.end,
           (unsigned long long)(im->globals_rng.end - im->globals_rng.start));
    printf("entry        : 0x%llx\n", (unsigned long long)im->entry);
    printf("main         : 0x%llx\n", (unsigned long long)im->main_addr);

    printf("\n== disassembly: %zu lines (%zu instructions) ==\n",
           im->n_dlines, im->n_irefs);
    for (size_t i = 0; i < im->n_dlines; i++) {
        const dline_t *d = &im->dlines[i];
        if (d->is_label)
            printf("%016llx %s\n", (unsigned long long)d->addr, d->text);
        else
            printf("  %llx:\t%s\n", (unsigned long long)d->addr, d->text);
    }

    printf("\n== line table: %zu entries (file: %s) ==\n",
           im->n_lmap, im->src_file);
    for (size_t i = 0; i < im->n_lmap; i++)
        printf("  0x%llx -> line %d\n",
               (unsigned long long)im->lmap[i].addr, im->lmap[i].line);

    printf("\n== source: %d lines ==\n", im->n_src);
}
