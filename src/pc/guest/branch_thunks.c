/* Indirect calls and jumps that may land in guest code, without DEP.
 *
 * Tables in the retail data image hold MIPS function addresses (the text
 * opcode handlers, callbacks an overlay installs), and native code calls
 * through them. Every unit the build compiles sends its indirect calls and
 * jumps through the thunks below: clang's -mretpoline-external-thunk and
 * GCC's -mindirect-branch=thunk-extern -mindirect-branch-register load the
 * target into a register and call or jump to __x86_indirect_thunk_<register>
 * (tools/pc/build_game32.py, tools/pc/build_mod.py). A target in guest memory
 * goes to Memories_GuestBranchResolver (image.c: the native function of that
 * address, or the MIPS interpreter's entry for an overlay callback), anything
 * else is jumped to as it is. A module function that C calls by name is
 * pinned to its guest address, so that call is direct and no thunk sees it:
 * the build gives each such name a host stub that pushes the address and
 * enters Memories_GuestBranchDirect below, which resolves it the same way.
 *
 * Before these, the only way in was the fault of executing guest RAM, which
 * needs DEP for the process: with DEP off, the MIPS bytes of a handler ran as
 * x86 code (the title's Options: the text handler at 0x80038b4c jumped to
 * 0x902b4950). The fault handler in image.c stays as the second net.
 *
 * The contract, for the compilers' code and for mods built by build_mod.py:
 * every register is kept, the target register too (a compiler may pick a
 * callee-saved one and keep it across the call), and the stack is as it was,
 * the caller's return address on top for a call, so a native function or
 * Memories_MipsThunk starts exactly as it would from a direct call (or from
 * the fault handler's redirect). Flags are not kept: they are dead at an
 * indirect call or jump. Nothing is stored below the stack pointer, where the
 * Windows interrupt clock pushes while the thread is suspended.
 *
 * Fast path: one test. Bits 21-28 and 30 are clear in every guest range
 * (physical 0x10000..0x200000, 0x80000000..0x80200000, 0xA0000000..
 * 0xA0200000); executable code never has them all clear, and the few host
 * addresses that do (0x20000000..0x20200000) come back from the resolver
 * unchanged. Slow path: a copy of the return address becomes the slot the
 * final `ret` takes the destination from (and what a stack walk reads as
 * the frame's return address), eax/ecx/edx/flags are saved, the resolver is
 * called on a 16-byte aligned stack, and everything is restored. */
#include "image.h"

void *(*Memories_GuestBranchResolver)(unsigned address);

#if defined(__i386__)
#ifdef _WIN32
#define SYMBOL(name) "_" #name
#define FUNCTION(name) ""
#define END(name) ""
#else
#define SYMBOL(name) #name
#define FUNCTION(name) ".type " #name ", @function\n"
#define END(name) ".size " #name ", . - " #name "\n"
#endif

/* `load` reads the target once eax/ecx/edx are saved and ebp is the frame:
 * the register itself, or for ebp its saved copy. */
#define THUNK(reg, load)                                                                  \
    ".p2align 4\n"                                                                        \
    ".globl " SYMBOL(__x86_indirect_thunk_##reg) "\n"                                     \
    FUNCTION(__x86_indirect_thunk_##reg)                                                  \
    SYMBOL(__x86_indirect_thunk_##reg) ":\n"                                              \
    "    testl $0x5fe00000, %" #reg "\n"                                                  \
    "    jz 1f\n"                                                                         \
    "    jmp *%" #reg "\n"                                                                \
    "1:  pushl (%esp)\n"                                                                  \
    "    pushl %ebp\n"                                                                    \
    "    movl %esp, %ebp\n"                                                               \
    "    pushfl\n"                                                                        \
    "    pushl %eax\n"                                                                    \
    "    pushl %ecx\n"                                                                    \
    "    pushl %edx\n"                                                                    \
    "    movl " load ", %eax\n"                                                           \
    "    jmp memories_branch_resolve\n"                                                   \
    END(__x86_indirect_thunk_##reg)

__asm__(".text\n"
        /* eax: the target. The frame: ebp+4 the slot, ebp-4 flags, then
         * eax, ecx, edx. */
        ".p2align 4\n"
        "memories_branch_resolve:\n"
        "    movl " SYMBOL(Memories_GuestBranchResolver) ", %ecx\n"
        "    testl %ecx, %ecx\n"
        "    jz 2f\n"
        "    andl $-16, %esp\n"
        "    subl $12, %esp\n"
        "    pushl %eax\n"
        "    call *%ecx\n"
        "2:  movl %eax, 4(%ebp)\n"
        "    leal -16(%ebp), %esp\n"
        "    popl %edx\n"
        "    popl %ecx\n"
        "    popl %eax\n"
        "    popfl\n"
        "    popl %ebp\n"
        "    ret\n"
        /* The entry of a host stub the build writes for a module function
         * that C calls by name (tools/pc/build_game32.py, guest_branches.c):
         * the stub pushes the guest address, which becomes the slot. */
        ".p2align 4\n"
        ".globl " SYMBOL(Memories_GuestBranchDirect) "\n"
        FUNCTION(Memories_GuestBranchDirect)
        SYMBOL(Memories_GuestBranchDirect) ":\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    pushfl\n"
        "    pushl %eax\n"
        "    pushl %ecx\n"
        "    pushl %edx\n"
        "    movl 4(%ebp), %eax\n"
        "    jmp memories_branch_resolve\n"
        END(Memories_GuestBranchDirect)
        THUNK(eax, "%eax")
        THUNK(ecx, "%ecx")
        THUNK(edx, "%edx")
        THUNK(ebx, "%ebx")
        THUNK(esi, "%esi")
        THUNK(edi, "%edi")
        THUNK(ebp, "(%ebp)"));
#elif defined(__arm__)

/* 32-bit ARM (Android) has no counterpart of -mindirect-branch=thunk-extern,
 * so there are no compiler thunks here: guest RAM is mapped without
 * PROT_EXEC (image.c), an indirect call through a pointer the retail data
 * image holds faults on its first instruction, and on_fault sends it to the
 * native function. That is the i386 port's "second net" doing the whole job,
 * and it is exact on ARM in a way it is not on x86: the return address is
 * already in lr, so redirecting pc leaves the callee's frame as a direct
 * call would. The cost is a signal per call through a guest pointer, which
 * only the retail tables take (text opcode handlers, overlay callbacks).
 *
 * A module function that C calls by name is pinned to its guest address, so
 * that call is direct and nothing would fault: the build gives each such name
 * a host stub that loads the address into r12 and enters the entry below
 * (tools/pc/build_game32.py, write_guest_branches).
 *
 * The contract of the i386 thunks holds: every register is kept but r12 (ip,
 * call-clobbered) and the flags, which are dead at an indirect call, and the
 * stack is as it was with lr still the caller's, so the native function or
 * Memories_MipsThunk starts exactly as it would from a direct call. The six
 * pushed registers keep sp 8-byte aligned for the resolver call, as AAPCS
 * wants. The resolver's own address comes off the literal pool as a link-time
 * offset (see state_arm.S) so that no text relocation is left behind. */
__asm__(".text\n"
        "    .arm\n"
        "    .globl Memories_GuestBranchDirect\n"
        "    .type Memories_GuestBranchDirect, %function\n"
        "Memories_GuestBranchDirect:\n"
        "    push {r0-r3, r12, lr}\n"
        "    ldr r1, .Lresolver_offset\n"
        ".Lresolver_pc:\n"
        "    add r1, pc, r1\n"
        "    ldr r1, [r1]\n"
        "    cmp r1, #0\n"
        "    beq 1f\n"
        "    mov r0, r12\n"
        "    blx r1\n"
        /* The resolved target replaces the saved r12 that 1: pops. */
        "    str r0, [sp, #16]\n"
        "1:  pop {r0-r3, r12, lr}\n"
        "    bx r12\n"
        ".Lresolver_offset:\n"
        "    .word Memories_GuestBranchResolver - (.Lresolver_pc + 8)\n"
        "    .size Memories_GuestBranchDirect, . - Memories_GuestBranchDirect\n");
#elif defined(__aarch64__)

/* AArch64 (Android), the same story as 32-bit ARM above: no compiler offers
 * an indirect-branch thunk here either, so there are none, guest RAM is
 * mapped without PROT_EXEC, and a call through a pointer the retail data
 * image holds faults on its first instruction and is redirected by on_fault
 * (image.c). AAPCS64 leaves the return address in x30, so the redirect
 * leaves the callee's frame exactly as a direct call would.
 *
 * A module function that C calls by name is pinned to its guest address, so
 * that call is direct and nothing would fault: the build gives each such
 * name a host stub that loads the address into x16 and enters the entry
 * below (tools/pc/build_game32.py, write_guest_branches).
 *
 * Kept across the resolver: x0-x7, the argument registers, x8, the indirect
 * result location, and x30. Not the floating-point argument registers, as
 * neither the i386 nor the 32-bit ARM entry keeps theirs: the game is
 * integer code, the console having no FPU, so nothing reaches a guest
 * pointer with an argument in v0-v7. x16 and x17 are IP0 and IP1, which a
 * call may clobber anyway. The twelve pushed words keep sp 16-byte aligned,
 * as AAPCS64 wants at a public interface. */
__asm__(".text\n"
        "    .globl Memories_GuestBranchDirect\n"
        "    .type Memories_GuestBranchDirect, %function\n"
        "Memories_GuestBranchDirect:\n"
        "    stp x0, x1, [sp, #-96]!\n"
        "    stp x2, x3, [sp, #16]\n"
        "    stp x4, x5, [sp, #32]\n"
        "    stp x6, x7, [sp, #48]\n"
        "    stp x8, x30, [sp, #64]\n"
        "    str x16, [sp, #80]\n"
        "    adrp x17, Memories_GuestBranchResolver\n"
        "    add x17, x17, :lo12:Memories_GuestBranchResolver\n"
        "    ldr x17, [x17]\n"
        "    cbz x17, 1f\n"
        "    mov w0, w16\n"
        "    blr x17\n"
        /* The resolved target replaces the saved x16 that 1: reloads. */
        "    str x0, [sp, #80]\n"
        "1:  ldr x16, [sp, #80]\n"
        "    ldp x8, x30, [sp, #64]\n"
        "    ldp x6, x7, [sp, #48]\n"
        "    ldp x4, x5, [sp, #32]\n"
        "    ldp x2, x3, [sp, #16]\n"
        "    ldp x0, x1, [sp], #96\n"
        "    br x16\n"
        "    .size Memories_GuestBranchDirect, . - Memories_GuestBranchDirect\n");
#endif
