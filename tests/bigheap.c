/* a large malloc is served by mmap (not brk) — it shows up in the heap
 * panel as a separate "mmap" region, and in the syscall log ('s') */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *small = malloc(64);       /* brk-based [heap] */
    char *big   = malloc(1 << 20);  /* mmap-based */
    if (!small || !big)
        return 1;
    strcpy(small, "small");
    big[0] = 'B';
    big[1] = 'I';
    big[2] = 'G';
    printf("small=%p big=%p\n", (void *)small, (void *)big);
    free(big);
    free(small);
    return 0;
}
