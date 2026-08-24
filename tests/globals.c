/* .data (initialized) vs .bss (zero) globals (spec 10.4, OSTEP ch.13) */
#include <stdio.h>

int  g_data = 0x11223344;  /* .data */
int  g_bss;                /* .bss  */
char g_buf[16];            /* .bss  */

int main(void)
{
    g_data += 1;
    g_bss = 0x55667788;
    for (int i = 0; i < 4; i++)
        g_buf[i] = (char)('A' + i);
    printf("data=%x bss=%x buf=%s\n", g_data, g_bss, g_buf);
    return 0;
}
