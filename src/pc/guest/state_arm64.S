/* VSync is the save-state boundary (see state.h), the AArch64 counterpart of
 * state_i386.S and state_arm.S. The entry records what the game caller
 * expects back -- the callee-saved registers, the stack pointer and the
 * return address -- and continues in the C implementation on the same frame.
 *
 * AAPCS64's callee-saved integer state is x19-x28, x29 (the frame pointer),
 * x30 (the return address) and sp: thirteen words, where i386 kept five and
 * 32-bit ARM ten. MemoriesStateEntry carries all three shapes.
 *
 * Reaching Memories_StateEntry: adrp/:lo12: is PC-relative and resolved at
 * link time for a hidden symbol (-fvisibility=hidden), so no relocation is
 * left for the loader and there is no text relocation, which Android
 * refuses. x16 and x17 are IP0 and IP1, call-clobbered. */
#define FUNCTION(name) .type name, %function
#define END(name) .size name, . - name
    .text
    .globl VSync
    FUNCTION(VSync)
VSync:
    adrp x16, Memories_StateEntry
    add x16, x16, :lo12:Memories_StateEntry
    stp x19, x20, [x16, #0]
    stp x21, x22, [x16, #16]
    stp x23, x24, [x16, #32]
    stp x25, x26, [x16, #48]
    stp x27, x28, [x16, #64]
    stp x29, x30, [x16, #80]
    mov x17, sp
    str x17, [x16, #96]
    b Memories_VSync
    END(VSync)

/* void Memories_StateReturn(const MemoriesStateEntry *entry, int value):
 * return `value` from the VSync call that `entry` describes. */
    .globl Memories_StateReturn
    FUNCTION(Memories_StateReturn)
Memories_StateReturn:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x16, [x0, #96]
    mov sp, x16
    mov w0, w1
    ret
    END(Memories_StateReturn)

/* void Memories_ContextSwitch(uintptr_t *from_sp, const uintptr_t *to_sp):
 * the ucontext stand-in, as on Windows and 32-bit ARM (state.c). Bionic has
 * no getcontext, makecontext or swapcontext. Keeps the callee-saved
 * registers on the stack being left and resumes the one whose stack pointer
 * `to_sp` holds. d8-d15 are callee-saved too under AAPCS64, which the other
 * two architectures' versions have no equivalent of; 160 bytes keeps sp
 * 16-byte aligned, as the ABI wants at every public interface. */
    .globl Memories_ContextSwitch
    FUNCTION(Memories_ContextSwitch)
Memories_ContextSwitch:
    stp x19, x20, [sp, #-160]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x16, sp
    str x16, [x0]
    ldr x16, [x1]
    mov sp, x16
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #160
    ret
    END(Memories_ContextSwitch)

    .section .note.GNU-stack,"",%progbits
