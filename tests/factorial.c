/* recursive factorial: watch stack frames pile up (spec 10.5) */
#include <stdio.h>

int fact(int n)
{
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

int main(void)
{
    int r = fact(5);
    printf("5! = %d\n", r);
    return 0;
}
