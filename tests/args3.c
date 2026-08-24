/* 3-argument call: RDI/RSI/RDX passing + call/ret stack motion (spec 10.6) */
#include <stdio.h>

int add3(int a, int b, int c)
{
    int s = a + b + c;
    return s;
}

int main(void)
{
    int r = add3(10, 20, 30);
    printf("sum = %d\n", r);
    return 0;
}
