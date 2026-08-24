/* analyzer.c - static analysis: objdump parsing + ELF section ranges + source
 *
 * Spec 6.1: disassembly and line table come from objdump output (robust
 * token-based parsing, not fixed-column sscanf); section ranges come from
 * parsing the ELF headers directly (Elf64_Ehdr/Elf64_Shdr).
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

/* ---------------- ELF section ranges ---------------- */

static int elf_analyze(trace_t *t, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cvisor: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "cvisor: %s: not an ELF file\n", path);
        close(fd);
        return -1;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "cvisor: mmap %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    const Elf64_Ehdr *eh = map;
    int rc = -1;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "cvisor: %s: not a 64-bit ELF binary\n", path);
        goto out;
    }
    if (eh->e_machine != EM_X86_64) {
        fprintf(stderr, "cvisor: %s: not an x86-64 binary\n", path);
        goto out;
    }
    if (eh->e_type == ET_DYN) {
        fprintf(stderr,
            "cvisor: %s is a PIE binary. Rebuild with:\n"
            "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n"
            "(PIE support is a Phase 3 item; objdump vs runtime addresses must "
            "match)\n", path);
        goto out;
    }
    if (eh->e_type != ET_EXEC) {
        fprintf(stderr, "cvisor: %s: not an executable (e_type=%d)\n",
                path, eh->e_type);
        goto out;
    }
    if (eh->e_shoff == 0 || eh->e_shstrndx == SHN_UNDEF) {
        fprintf(stderr, "cvisor: %s: no section headers (stripped?)\n", path);
        goto out;
    }

    const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)map + eh->e_shoff);
    const char *shstr = (const char *)map + sh[eh->e_shstrndx].sh_offset;

    range_t data = {0, 0}, bss = {0, 0};
    for (int i = 0; i < eh->e_shnum; i++) {
        const char *name = shstr + sh[i].sh_name;
        if (strcmp(name, ".text") == 0) {
            t->text.start = sh[i].sh_addr;
            t->text.end   = sh[i].sh_addr + sh[i].sh_size;
        } else if (strcmp(name, ".data") == 0) {
            data.start = sh[i].sh_addr;
            data.end   = sh[i].sh_addr + sh[i].sh_size;
        } else if (strcmp(name, ".bss") == 0) {
            bss.start = sh[i].sh_addr;
            bss.end   = sh[i].sh_addr + sh[i].sh_size;
        }
    }
    if (t->text.start == 0) {
        fprintf(stderr, "cvisor: %s: no .text section\n", path);
        goto out;
    }

    /* covering range of .data + .bss (they are adjacent in practice) */
    if (data.start && bss.start) {
        t->globals_rng.start = data.start < bss.start ? data.start : bss.start;
        t->globals_rng.end   = data.end   > bss.end   ? data.end   : bss.end;
    } else if (data.start) {
        t->globals_rng = data;
    } else {
        t->globals_rng = bss;
    }
    if (t->globals_rng.end - t->globals_rng.start > CV_GLOBALS_MAX)
        t->globals_rng.end = t->globals_rng.start + CV_GLOBALS_MAX;

    t->entry = eh->e_entry;
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

static int add_dline(trace_t *t, uint64_t addr, int is_label, const char *text)
{
    if (t->n_dlines % 1024 == 0) {
        dline_t *nd = realloc(t->dlines,
                              (t->n_dlines + 1024) * sizeof(dline_t));
        if (!nd)
            return -1;
        t->dlines = nd;
    }
    dline_t *d = &t->dlines[t->n_dlines];
    d->addr = addr;
    d->is_label = is_label;
    d->text = xstrdup_trim(text);
    if (!d->text)
        return -1;
    t->n_dlines++;
    return 0;
}

static int parse_disasm(trace_t *t, const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "objdump -d --no-show-raw-insn '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "cvisor: failed to run objdump -d\n");
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
                if (add_dline(t, addr, 1, label) < 0)
                    goto oom;
                if (strncmp(lt, "<main>:", 7) == 0)
                    t->main_addr = addr;
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
        if (add_dline(t, addr, 0, end) < 0)
            goto oom;

        if (t->n_irefs == iref_cap) {
            iref_cap = iref_cap ? iref_cap * 2 : 1024;
            insn_ref_t *nr = realloc(t->irefs, iref_cap * sizeof(insn_ref_t));
            if (!nr)
                goto oom;
            t->irefs = nr;
        }
        t->irefs[t->n_irefs].addr = addr;
        t->irefs[t->n_irefs].dline_idx = (int32_t)(t->n_dlines - 1);
        t->n_irefs++;
    }
    pclose(fp);

    if (t->n_irefs == 0) {
        fprintf(stderr, "cvisor: objdump -d produced no instructions "
                        "(is objdump installed?)\n");
        return -1;
    }
    return 0;
oom:
    pclose(fp);
    fprintf(stderr, "cvisor: out of memory parsing disassembly\n");
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

/*
 * Row formats vary across binutils versions; per spec, don't sscanf fixed
 * columns.  Tokenize and use: token starting with "0x" = address, the token
 * right before it = line number ("-" = end of sequence), first token = file.
 */
#define CV_MAX_CUS 64

static int parse_decodedline(trace_t *t, const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "objdump --dwarf=decodedline '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "cvisor: failed to run objdump --dwarf=decodedline\n");
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
            if (n && t->src_file[0] == '\0')
                snprintf(t->src_file, sizeof(t->src_file), "%s", s);
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

        if (t->n_lmap == cap) {
            cap = cap ? cap * 2 : 512;
            lmap_t *nl = realloc(t->lmap, cap * sizeof(lmap_t));
            int *nf = realloc(fids, cap * sizeof(int));
            if (!nl || !nf) {
                if (nl)
                    t->lmap = nl;
                if (nf)
                    fids = nf;
                free(fids);
                pclose(fp);
                fprintf(stderr, "cvisor: out of memory parsing line table\n");
                return -1;
            }
            t->lmap = nl;
            fids = nf;
        }
        t->lmap[t->n_lmap].addr = addr;
        t->lmap[t->n_lmap].line = lno;
        fids[t->n_lmap] = fid;
        t->n_lmap++;
    }
    pclose(fp);

    if (t->n_lmap == 0) {
        free(fids);
        fprintf(stderr,
            "cvisor: no DWARF line info in %s. Rebuild with:\n"
            "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n",
            path);
        return -1;
    }

    /* pick the CU that owns main (fallback: the one with the most rows);
     * rows from other CUs (crt startup etc.) become "no mapping" */
    int chosen = -1;
    if (t->main_addr) {
        uint64_t best = 0;
        for (size_t i = 0; i < t->n_lmap; i++) {
            if (t->lmap[i].addr <= t->main_addr && t->lmap[i].addr >= best &&
                t->lmap[i].line != -1) {
                best = t->lmap[i].addr;
                chosen = fids[i];
            }
        }
    }
    if (chosen < 0 && n_files > 0) {
        int counts[CV_MAX_CUS] = {0};
        for (size_t i = 0; i < t->n_lmap; i++)
            counts[fids[i]]++;
        chosen = 0;
        for (int f = 1; f < n_files; f++)
            if (counts[f] > counts[chosen])
                chosen = f;
    }
    if (chosen >= 0) {
        for (size_t i = 0; i < t->n_lmap; i++)
            if (fids[i] != chosen)
                t->lmap[i].line = -1;
        snprintf(t->src_file, sizeof(t->src_file), "%.255s", files[chosen]);
    }
    free(fids);

    qsort(t->lmap, t->n_lmap, sizeof(lmap_t), lmap_cmp);
    return 0;
}

/* ---------------- source loading ---------------- */

static FILE *open_source(const trace_t *t, const char *target_path)
{
    FILE *f = fopen(t->src_file, "r");
    if (f)
        return f;

    /* try basename of src_file next to the target binary, then in cwd */
    char tmp[512], dirbuf[512];
    snprintf(tmp, sizeof(tmp), "%s", t->src_file);
    const char *base = basename(tmp);

    snprintf(dirbuf, sizeof(dirbuf), "%s", target_path);
    char joined[1200];
    snprintf(joined, sizeof(joined), "%s/%s", dirname(dirbuf), base);
    f = fopen(joined, "r");
    if (f)
        return f;
    return fopen(base, "r");
}

static int load_source(trace_t *t, const char *target_path)
{
    if (t->src_file[0] == '\0')
        return -1;
    FILE *f = open_source(t, target_path);
    if (!f) {
        fprintf(stderr, "cvisor: warning: source file '%s' not found; "
                        "source panel will be empty\n", t->src_file);
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

        if (t->n_src == cap) {
            cap = cap ? cap * 2 : 256;
            char **ns = realloc(t->src, (size_t)cap * sizeof(char *));
            if (!ns) {
                fclose(f);
                return -1;
            }
            t->src = ns;
        }
        t->src[t->n_src] = strdup(expanded);
        if (!t->src[t->n_src]) {
            fclose(f);
            return -1;
        }
        t->n_src++;
    }
    fclose(f);
    return 0;
}

/* ---------------- entry points ---------------- */

int analyze(trace_t *t, const char *target_path)
{
    if (elf_analyze(t, target_path) < 0)
        return -1;
    if (parse_disasm(t, target_path) < 0)
        return -1;
    if (parse_decodedline(t, target_path) < 0)
        return -1;
    load_source(t, target_path); /* soft failure: TUI degrades gracefully */
    return 0;
}

void analyze_dump(const trace_t *t)
{
    printf("== ELF ==\n");
    printf(".text        : 0x%llx - 0x%llx\n",
           (unsigned long long)t->text.start, (unsigned long long)t->text.end);
    printf(".data+.bss   : 0x%llx - 0x%llx (%llu bytes)\n",
           (unsigned long long)t->globals_rng.start,
           (unsigned long long)t->globals_rng.end,
           (unsigned long long)(t->globals_rng.end - t->globals_rng.start));
    printf("entry        : 0x%llx\n", (unsigned long long)t->entry);
    printf("main         : 0x%llx\n", (unsigned long long)t->main_addr);

    printf("\n== disassembly: %zu lines (%zu instructions) ==\n",
           t->n_dlines, t->n_irefs);
    for (size_t i = 0; i < t->n_dlines; i++) {
        const dline_t *d = &t->dlines[i];
        if (d->is_label)
            printf("%016llx %s\n", (unsigned long long)d->addr, d->text);
        else
            printf("  %llx:\t%s\n", (unsigned long long)d->addr, d->text);
    }

    printf("\n== line table: %zu entries (file: %s) ==\n",
           t->n_lmap, t->src_file);
    for (size_t i = 0; i < t->n_lmap; i++)
        printf("  0x%llx -> line %d\n",
               (unsigned long long)t->lmap[i].addr, t->lmap[i].line);

    printf("\n== source: %d lines ==\n", t->n_src);
}
