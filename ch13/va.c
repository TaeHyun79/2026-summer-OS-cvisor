//
// Created by Taehyun on 9/5/26.
//
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    printf("Location of code : %p\n", main);
    printf("Location of heap : %p\n", malloc(100e6));
    int x = 3;
    printf("Location of stack: %p\n", &x);
    return x;
}
