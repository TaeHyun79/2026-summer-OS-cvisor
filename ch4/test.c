//
// Created by Taehyun on 8/25/26.
//
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int rc = fork();
    if (rc < 0) {
        fprintf(stderr, "fork failed\n");
        exit(1);
    } else if (rc == 0) {
        printf("child (pid: %d)\n", (int) getpid());
    } else {
        printf("parent of %d (pid: %d)\n", rc, (int) getpid());
    }

    return 0;
}