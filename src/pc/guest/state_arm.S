/* VSync is the save-state boundary (see state.h), the 32-bit ARM counterpart
 * of state_i386.S. The entry records what the game caller expects back -- the
 * callee-saved registers, the stack pointer and the return address -- and
 * continues in the C implementation on the same frame.
 *
 * Where the i386 entry saves five words and lets the saved esp stand for the
 * return address on the stack, AAPCS keeps the return address in lr, so this
 * one saves ten: r4-r11, sp and lr. MemoriesStateEntry carries both shapes.
 *
 * Reaching Memories_StateEntry: the Android build is one shared library whose
 * symbols are hidden (-fvisibility=hidden), so an offset from the literal
 * pool to the global is a link-time constant. `add r12, pc, r12` then forms
 * the address with no relocation left for the loader, which a text
 * relocation -- what `ldr r12, =Memories_StateEntry` would need -- would be,
 * and Android rejects those. r12 (ip) is call-clobbered, so spending it here
 * costs the caller nothing. */
#define FUNCTION(name) .type name, %function
#define END(name) .size name, . - name
    .text
    .arm
    .globl VSync
    FUNCTION(VSync)
VSync:
    ldr r12, .Lentry_offset
.Lentry_pc:
    add r12, pc, r12
    stmia r12, {r4-r11, sp, lr}
    b Memories_VSync
.Lentry_offset:
    .word Memories_StateEntry - (.Lentry_pc + 8)
    END(VSync)

/* void Memories_StateReturn(const MemoriesStateEntry *entry, int value):
 * return `value` from the VSync call that `entry` describes. */
    .globl Memories_StateReturn
    FUNCTION(Memories_StateReturn)
Memories_StateReturn:
    mov r2, r1
    ldmia r0, {r4-r11, sp, lr}
    mov r0, r2
    bx lr
    END(Memories_StateReturn)

/* void Memories_ContextSwitch(uint32_t *from_sp, const uint32_t *to_sp):
 * the ucontext stand-in, as on Windows (state.c). Bionic has no getcontext,
 * makecontext or swapcontext -- Android never shipped them -- so the game
 * reaches its fixed stack the way the Windows port does. Keeps the
 * callee-saved registers on the stack being left and resumes the one whose
 * stack pointer `to_sp` holds. Ten words, so sp stays 8-byte aligned; r12 is
 * pushed only to make the count even and is not otherwise live here. */
    .globl Memories_ContextSwitch
    FUNCTION(Memories_ContextSwitch)
Memories_ContextSwitch:
    push {r4-r11, r12, lr}
    str sp, [r0]
    ldr sp, [r1]
    pop {r4-r11, r12, lr}
    bx lr
    END(Memories_ContextSwitch)

    .section .note.GNU-stack,"",%progbits
