/* Psx_setjmp/Psx_longjmp for 32-bit ARM (Android), the counterpart of
 * setjmp_i386.S. The game's jmp_buf is the Psy-Q int[12] at a fixed guest
 * address, too small for the host libc's; AAPCS needs ten of those words:
 * r4-r11, the stack pointer and the return address. Like the i386 pair this
 * keeps the integer callee-saved state only. The game is integer code (the
 * console has no FPU), so no VFP register is live across a setjmp, and
 * d8-d15 would not fit in the twelve words anyway.
 *
 * The file is ARM (A32), as the whole Android build is (-marm, see
 * tools/pc/build_game32.py): the guest glue reaches its own hidden globals
 * with `add rN, pc, rN` off a link-time offset, which A32 has and Thumb-2
 * does not, and keeping one instruction set spares every stub an
 * interworking veneer. */
#define FUNCTION(name) .type name, %function
#define END(name) .size name, . - name
    .text
    .arm
    .globl Psx_setjmp
    FUNCTION(Psx_setjmp)
Psx_setjmp:
    stmia r0, {r4-r11, sp, lr}
    mov r0, #0
    bx lr
    END(Psx_setjmp)

    .globl Psx_longjmp
    FUNCTION(Psx_longjmp)
Psx_longjmp:
    /* longjmp(buffer, 0) returns 1, as the C library's does. */
    cmp r1, #0
    moveq r1, #1
    ldmia r0, {r4-r11, sp, lr}
    mov r0, r1
    bx lr
    END(Psx_longjmp)

    .section .note.GNU-stack,"",%progbits
