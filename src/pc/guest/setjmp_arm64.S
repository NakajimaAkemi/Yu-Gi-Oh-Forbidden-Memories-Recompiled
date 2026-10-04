/* Psx_setjmp/Psx_longjmp for AArch64 (Android), where the other two
 * architectures' approach does not fit.
 *
 * The game's jmp_buf is the Psy-Q int[12] at a fixed guest address: 48 bytes.
 * i386 needed six words of it and 32-bit ARM ten, but AAPCS64's callee-saved
 * state is x19-x28, x29, x30 and sp -- 104 bytes, which does not fit, and
 * widening the buffer is not an option because it is retail guest memory at
 * a retail address.
 *
 * So the registers live in Memories_JumpState, a native area of their own
 * (state.c), and the guest buffer keeps only a token saying it was set. One
 * area is enough because the game has exactly one jmp_buf: setjmp is called
 * once, in Main_Init, and the three longjmps all name D_800E9DC0. If a
 * second one ever appears, this becomes a small table keyed by the buffer's
 * guest address, and the token becomes its index.
 *
 * The token is checked rather than trusted: a longjmp through a buffer that
 * no setjmp filled would otherwise restore whatever the area happened to
 * hold, so Memories_JumpUnset reports it and stops.
 *
 * Reaching the area: adrp/:lo12: is PC-relative and resolved at link time
 * for a hidden symbol (-fvisibility=hidden), so nothing is left for the
 * loader and there is no text relocation, which Android would refuse. x16
 * and x17 are IP0 and IP1, call-clobbered, so using them costs the caller
 * nothing. */
#define FUNCTION(name) .type name, %function
#define END(name) .size name, . - name
/* Must match MEMORIES_JUMP_TOKEN in state.c. */
#define TOKEN_LOW 0x4236
#define TOKEN_HIGH 0x4D4A
    .text
    .globl Psx_setjmp
    FUNCTION(Psx_setjmp)
Psx_setjmp:
    adrp x16, Memories_JumpState
    add x16, x16, :lo12:Memories_JumpState
    stp x19, x20, [x16, #0]
    stp x21, x22, [x16, #16]
    stp x23, x24, [x16, #32]
    stp x25, x26, [x16, #48]
    stp x27, x28, [x16, #64]
    stp x29, x30, [x16, #80]
    mov x17, sp
    str x17, [x16, #96]
    /* The buffer says only that it was set; the state is in the area. */
    movz w17, #TOKEN_LOW
    movk w17, #TOKEN_HIGH, lsl #16
    str w17, [x0]
    mov w0, #0
    ret
    END(Psx_setjmp)

    .globl Psx_longjmp
    FUNCTION(Psx_longjmp)
Psx_longjmp:
    ldr w17, [x0]
    movz w16, #TOKEN_LOW
    movk w16, #TOKEN_HIGH, lsl #16
    cmp w17, w16
    b.ne Memories_JumpUnset     /* does not return */
    adrp x16, Memories_JumpState
    add x16, x16, :lo12:Memories_JumpState
    ldp x19, x20, [x16, #0]
    ldp x21, x22, [x16, #16]
    ldp x23, x24, [x16, #32]
    ldp x25, x26, [x16, #48]
    ldp x27, x28, [x16, #64]
    ldp x29, x30, [x16, #80]
    ldr x17, [x16, #96]
    mov sp, x17
    /* longjmp(buffer, 0) returns 1, as the C library's does. */
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret
    END(Psx_longjmp)

    .section .note.GNU-stack,"",%progbits
