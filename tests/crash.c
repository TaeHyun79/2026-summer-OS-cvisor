/* null pointer dereference: rewind-to-the-crash scenario (spec 10.7) */
#include <stdio.h>

int main(void)
{
    int *p = NULL;
    printf("about to crash\n");
    *p = 42;   /* SIGSEGV here */
    return 0;
}
