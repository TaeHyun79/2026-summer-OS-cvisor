/* pure assembly, no libc: entry is _start, output via raw syscalls.
 * Every recorded step is hand-written code (skipped == 0), and every
 * kernel entry in the 's' panel is a `syscall` instruction written here.
 *
 * build: as -g -o hello.o hello.s && ld -o hello hello.o
 * (as -g is what produces the DWARF line table cvisor requires) */
        .section .rodata
msg:    .ascii  "hello from asm\n"
        .set    msglen, . - msg

        .section .data
counter:
        .quad   0

        .text
        .globl  _start
_start:
        movq    $3, %rbx                /* loop counter in a callee-saved reg */
.Lloop:
        movq    $1, %rax                /* sys_write */
        movq    $1, %rdi                /* fd = stdout */
        leaq    msg(%rip), %rsi
        movq    $msglen, %rdx
        syscall

        incq    counter(%rip)           /* touch .data so the globals panel moves */
        decq    %rbx
        jnz     .Lloop

        movq    $60, %rax               /* sys_exit */
        movq    counter(%rip), %rdi     /* status = number of lines printed */
        syscall
