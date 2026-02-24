/*
 * pal_start-x86_64.S
 *
 * Entry point for WAMR PAL running in VMPL1.
 * Based on gramine-svsm/pal/src/host/svsm/arch/x86_64/pal_start.S
 *
 * SVSM sets VMSA.rip = pal_start (via ELF e_entry),
 * VMSA.rsp/rbp = TP_STACK_START_VADDR + 8*4096.
 * We are in VMPL1, Ring 3, no libc, no OS.
 */

/* Mark stack as non-executable (suppresses ld warning) */
.section .note.GNU-stack, "", @progbits

.text
.global pal_start
.type pal_start, @function
.align 0x10
pal_start:
    .cfi_startproc
    xorq %rbp, %rbp    /* mark the last stack frame with RBP == 0 (for debuggers) */
    andq $~0xf, %rsp   /* align stack to 16 bytes */
    call wamr_pal_main  /* our entry function */
    /* Unreachable - wamr_pal_main calls pal_svsm_exit() */
    .cfi_endproc
