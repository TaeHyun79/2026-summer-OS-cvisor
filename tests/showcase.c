/* showcase: every cvisor pane changes over one short run.
 *
 * Watch it with:   ./cvisor --from-main tests/showcase
 * (wide terminal recommended — all memory panes visible at once)
 *
 * Phases, in source order:
 *   1. counting loop     — registers/EFLAGS flicker, .data global ticks up
 *   2. fib(6) recursion  — stack pane: frames pile up and unwind
 *   3. .bss writes       — globals pane: zeros become values ("ABCD")
 *   4. small malloc      — heap pane: brk region bytes fill 00 01 02 ...
 *   5. big malloc (1 MB) — heap pane: an mmap region appears
 *   6. first printf      — globals pane: GOT entry flips (lazy binding),
 *                          syscall log: write(), output panel: text arrives
 *   7. free(big)         — heap pane: the mmap region disappears
 *   8. final global      — .data ends as 0xdeadbeef
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int         g_counter = 0x11111111;   /* .data: watch it increment */
long        g_accum;                  /* .bss: starts as zeros */
char        g_msg[8];                 /* .bss: becomes "ABCD" */
const char *g_ro = "from-rodata";     /* the string lives in .rodata */

/* recursion: stack frames + conditional branches (EFLAGS ZF/SF) */
static int fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void)
{
    /* 1: registers + .data */
    int x = 0;
    for (int i = 0; i < 5; i++) {
        x += i;
        g_counter++;
    }

    /* 2: stack */
    int f = fib(6);

    /* 3: .bss */
    g_accum = (long)f * 1000 + x;
    for (int i = 0; i < 4; i++)
        g_msg[i] = (char)('A' + i);

    /* 4: brk heap, byte by byte */
    unsigned char *small = malloc(32);
    if (!small)
        return 1;
    for (int i = 0; i < 32; i++)
        small[i] = (unsigned char)i;

    /* 5: mmap heap */
    char *big = malloc(1 << 20);
    if (!big)
        return 1;
    memcpy(big, "BIG-REGION", 11);

    /* 6: GOT flip + syscall + program output */
    printf("fib=%d x=%d ro=%s big=%s\n", f, x, g_ro, big);
    printf("msg=%s accum=%ld\n", g_msg, g_accum);

    /* 7: the mmap region vanishes from the heap pane */
    free(big);
    free(small);

    /* 8: parting change in .data */
    g_counter = 0xdeadbeef;
    return 0;
}
