/* bounded version of OSTEP intro/mem.c: heap pointer + value updates
 * (spec 10.1; with ASLR off the address is identical across runs) */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int *p = malloc(sizeof(int));
    if (p == NULL)
        return 1;
    printf("addr of p: %p\n", (void *)p);
    *p = 0;
    for (int i = 0; i < 5; i++) {
        *p = *p + 1;
        printf("value of p: %d\n", *p);
    }
    free(p);
    return 0;
}
