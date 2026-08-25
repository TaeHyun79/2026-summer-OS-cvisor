/* OSTEP ch.6, extra: what a context switch actually *is*
 *
 * xv6's swtch() ported from i386 to x86-64, plus a two-"thread" driver that
 * switches back and forth in user space.  No libc, no kernel involvement:
 * the switch here is nothing but saving callee-saved registers + RSP + the
 * return address into one struct and loading them from another.
 *
 * Contrast with ch6/ctx_switch.c, which measures the cost of the *kernel's*
 * switch between two processes.  That one you can only time; this one you
 * can single-step through.
 *
 * Port notes (the original is 32-bit xv6 swtch.S):
 *   - i386 passes both arguments on the stack, so the original reads
 *     `movl 4(%esp), %eax` twice — and the second read means a *different*
 *     argument because the intervening `popl` moved %esp.  In the SysV
 *     AMD64 ABI the arguments arrive in %rdi and %rsi, so that subtlety
 *     disappears.
 *   - callee-saved set differs: i386 has ebx/esi/edi/ebp, x86-64 has
 *     rbx/rbp/r12/r13/r14/r15.  esi/edi are caller-saved here (they are
 *     argument registers), so they are NOT part of the context.
 *
 * usage:
 *   make ch6                     # builds this with `as -g` + `ld`
 *   ./ch6/swtch                  # runs it standalone
 *   ./cvisor ch6/swtch           # single-step the switch
 *
 * What to watch in the TUI:
 *   - registers panel: RBX is A's round counter, R12 is B's.  Each `ret`
 *     at the end of swtch makes both change at once — that IS the switch.
 *   - globals panel (Tab twice): ctx_a and ctx_b are the first 128 bytes.
 *     Watch swtch write the saved RIP/RSP into them.
 *   - 's' panel: only the write(2) calls appear.  The switch itself costs
 *     zero syscalls, which is the whole point.
 */

/* struct context {
 *     uint64_t rip;   //  0   return address
 *     uint64_t rsp;   //  8
 *     uint64_t rbx;   // 16   callee-saved from here down
 *     uint64_t rbp;   // 24
 *     uint64_t r12;   // 32
 *     uint64_t r13;   // 40
 *     uint64_t r14;   // 48
 *     uint64_t r15;   // 56
 * };                  // 64 bytes
 */

        .section .rodata
msg_a:  .ascii  "A running\n"
        .set    msg_a_len, . - msg_a
msg_b:  .ascii  "    B running\n"
        .set    msg_b_len, . - msg_b

        .section .data
        .balign 8
/* A's context: left zeroed, swtch fills it on the first switch away from A */
ctx_a:  .quad   0, 0, 0, 0, 0, 0, 0, 0
/* B's context: _start hand-builds rip/rsp so the first switch *into* B
 * lands on thread_b running on B's own stack */
ctx_b:  .quad   0, 0, 0, 0, 0, 0, 0, 0

        .section .bss
        .balign 16
b_stack:
        .space  4096
b_stack_top:

        .text

/* ---------------------------------------------------------------------
 * void swtch(struct context *old, struct context *new);
 *
 * Save current register context in old
 * and then load register context from new.
 * --------------------------------------------------------------------- */
        .globl  swtch
swtch:
        # Save old registers                    (%rdi = old)
        popq    0(%rdi)         # save the old IP
        movq    %rsp,  8(%rdi)  # and stack
        movq    %rbx, 16(%rdi)  # and other registers
        movq    %rbp, 24(%rdi)
        movq    %r12, 32(%rdi)
        movq    %r13, 40(%rdi)
        movq    %r14, 48(%rdi)
        movq    %r15, 56(%rdi)

        # Load new registers                    (%rsi = new)
        movq    56(%rsi), %r15  # restore other registers
        movq    48(%rsi), %r14
        movq    40(%rsi), %r13
        movq    32(%rsi), %r12
        movq    24(%rsi), %rbp
        movq    16(%rsi), %rbx
        movq     8(%rsi), %rsp  # stack is switched here
        pushq   0(%rsi)         # return addr put in place
        ret                     # finally return into new ctxt

/* ---------------------------------------------------------------------
 * write(1, %rsi, %rdx).  Clobbers %rax/%rdi/%rcx/%r11 — all caller-saved,
 * so it survives being called from either "thread".
 * --------------------------------------------------------------------- */
put:
        movq    $1, %rax        # sys_write
        movq    $1, %rdi        # fd = stdout
        syscall
        ret

/* ---------------------------------------------------------------------
 * thread A — the one the kernel starts us on, using the process stack
 * --------------------------------------------------------------------- */
        .globl  _start
_start:
        leaq    thread_b(%rip), %rax
        movq    %rax, ctx_b+0(%rip)     # ctx_b.rip = thread_b
        leaq    b_stack_top(%rip), %rax
        movq    %rax, ctx_b+8(%rip)     # ctx_b.rsp = top of B's stack

        movq    $3, %rbx                # A's round counter (callee-saved)

.La_loop:
        leaq    msg_a(%rip), %rsi
        movq    $msg_a_len, %rdx
        call    put

        leaq    ctx_a(%rip), %rdi       # swtch(&ctx_a, &ctx_b)
        leaq    ctx_b(%rip), %rsi
        call    swtch                   # B runs; we resume on the next line

        decq    %rbx
        jnz     .La_loop

        movq    $60, %rax               # sys_exit
        xorq    %rdi, %rdi
        syscall

/* ---------------------------------------------------------------------
 * thread B — never returns; A ends the process once its rounds are done.
 * Entered the first time via swtch's `pushq 0(%rsi); ret`, and on every
 * later switch at the instruction after its own `call swtch`.
 * --------------------------------------------------------------------- */
thread_b:
.Lb_loop:
        incq    %r12                    # B's own counter (callee-saved)

        leaq    msg_b(%rip), %rsi
        movq    $msg_b_len, %rdx
        call    put

        leaq    ctx_b(%rip), %rdi       # swtch(&ctx_b, &ctx_a)
        leaq    ctx_a(%rip), %rsi
        call    swtch                   # A runs; we resume on the next line
        jmp     .Lb_loop
