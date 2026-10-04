#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "image.h"
#include "mips.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/debug/profile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

/* What the model requires is that a pointer the game stores is four bytes
 * wide. An ILP32 host gives that for nothing; a 64-bit one gives it through
 * G32 (src/port_ptr.h), where the host's own pointers stay eight. */
#include "port_ptr.h"
#if defined(__LP64__)
struct MemoriesGuestPointer { void *G32 p; };
_Static_assert(sizeof(struct MemoriesGuestPointer) == 4,
               "a 64-bit build needs G32 to give four-byte guest pointers");
#else
_Static_assert(sizeof(void *) == 4, "the guest image model requires an ILP32 build");
#endif

/* The first 64 KiB. On the console that is kernel RAM, and retail code reaches
 * it through null pointers: CardList_CreateSlotTextBox clears a flag in
 * box->field_28 one call before that object exists, a read-modify-write of
 * address 8 that nothing notices. Hosts do not let a process map page zero,
 * so such an access faults; the handler then points the instruction's base
 * register at `low_memory` (the same pages guest RAM has at 0x80000000),
 * single-steps it, and puts the register back unless the instruction itself
 * replaced it. Each site is reported once. */
#ifndef _WIN32
static unsigned char *low_memory;
#else
/* Which 64 KiB pieces of the physical mirror Memories_GuestMap could map;
 * Windows holds the others (see there). */
static unsigned char low_piece_mapped[MEMORIES_GUEST_RAM_SIZE / 0x10000u];
#endif
#if defined(__i386__)
/* The x86 repair in flight: the register pointed at `low_memory` while the
 * one faulting instruction is single-stepped. The ARM handler performs the
 * transfer itself and needs none of this (see on_fault). */
static struct {
    int active, reg; /* reg: the ModRM register number, 0 (EAX) to 7 (EDI) */
    uint32_t original, patched;
} low_fixup;
#endif

static void report_low_access(uint32_t eip, uint32_t address)
{
    static uint32_t seen[32];
    static unsigned count;
    char text[128];
    unsigned i;
    int length;
    for (i = 0; i < count; i++) {
        if (seen[i] == eip) {
            return;
        }
    }
    if (count < sizeof(seen) / sizeof(seen[0])) {
        seen[count++] = eip;
    }
    length = snprintf(text, sizeof(text), "memories-pc: null-pointer access to 0x%04x at eip 0x%08x goes to kernel RAM, as on the console\n",
                      (unsigned)address, (unsigned)eip);
    (void)!write(2, text, (size_t)length);
}

#if defined(__i386__)
/* Which register (ModRM number) does the faulting instruction address memory
 * through? -1 for none. */
#define REGISTER_ESI 6
#define REGISTER_EDI 7
static int low_access_register(const unsigned char *code, uint32_t esi)
{
    unsigned modrm, base;
    while (*code == 0x66 || *code == 0xf2 || *code == 0xf3 || *code == 0x2e || *code == 0x36 || *code == 0x3e ||
           *code == 0x26) {
        code++;
    }
    if ((*code >= 0xa4 && *code <= 0xa7) || *code == 0xaa || *code == 0xab) { /* string moves and stores */
        if (*code != 0xaa && *code != 0xab && esi < 0x10000u) {
            return REGISTER_ESI;
        }
        return REGISTER_EDI;
    }
    code += *code == 0x0f ? 2 : 1;
    modrm = *code++;
    if (modrm >> 6 == 3 || ((modrm >> 6) == 0 && (modrm & 7) == 5)) {
        return -1; /* register operand, or an absolute address */
    }
    base = modrm & 7;
    if (base == 4) {
        unsigned sib = *code;
        base = sib & 7;
        if (base == 5 && modrm >> 6 == 0) {
            return -1;
        }
    }
    return (int)base;
}
#endif /* __i386__ */

/* Tables in the retail data image hold MIPS function addresses, and native
 * code calls through them. The build generates Memories_FunctionMap (guest
 * address -> native function, sorted); a call to such an address resumes in
 * the native function. The caller's return address and cdecl arguments are
 * already on the stack, so the redirect is transparent. Two ways lead here:
 * the indirect-branch thunks every unit is compiled to use (branch_thunks.c,
 * through guest_branch_target below), and, as the second net, the fault of
 * executing guest RAM, which is mapped without execute permission
 * (on_guest_exception, on_fault). Anything else is fatal.
 * Returns where to resume, or NULL. */
static int in_guest_ram(uint32_t address)
{
    return (address >= 0x10000u && address < MEMORIES_GUEST_RAM_SIZE) ||
           address - MEMORIES_GUEST_RAM < MEMORIES_GUEST_RAM_SIZE || address - 0xa0000000u < MEMORIES_GUEST_RAM_SIZE;
}

static void *guest_call_target(uint32_t address)
{
    size_t low = 0, high = Memories_FunctionMapCount;
    /* Through KSEG1 or the physical address, the console runs the same
     * code; the map and the overlay slots are keyed by KSEG0. */
    if (in_guest_ram(address)) {
        address = MEMORIES_GUEST_RAM | (address & (MEMORIES_GUEST_RAM_SIZE - 1u));
    }
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (Memories_FunctionMap[middle].guest < address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    for (; low < Memories_FunctionMapCount && Memories_FunctionMap[low].guest == address; low++) {
        const MemoriesGuestFunction *entry = &Memories_FunctionMap[low];
        if (Memories_ModuleIsResident(entry->bank, entry->identifier)) {
            return (void *)(uintptr_t)entry->host;
        }
    }
    if (Memories_MipsInOverlay(address)) {
        /* A callback into a loaded overlay: run it interpreted. */
        Memories_MipsThunkTarget = address;
        return (void *)(uintptr_t)Memories_MipsThunk;
    }
    return NULL;
}

static void report_guest_fault(uint32_t address, uint32_t eip)
{
    char text[128];
    int length;
    if (eip == address) {
        length = snprintf(text, sizeof(text), "memories-pc: call into guest code at 0x%08x, which has no native function\n",
                          (unsigned)address);
    } else {
        length = snprintf(text, sizeof(text), "memories-pc: bad memory access at 0x%08x (eip 0x%08x)\n",
                          (unsigned)address, (unsigned)eip);
    }
    (void)!write(2, text, (size_t)length);
}

/* Memories_GuestBranchResolver: where an indirect call or jump the thunks
 * caught goes. Outside guest RAM and its mirrors (and below 0x10000, so that
 * a call through a null pointer still faults as one; on Windows also in the
 * pieces of the physical mirror that Windows holds, which are not guest
 * RAM), the address itself. A guest address with no native function is
 * never jumped to: with DEP off its MIPS bytes would run as x86 code. */
static void *guest_branch_target(unsigned address)
{
    char text[64];
    void *target;
    if (!in_guest_ram(address)) {
        return (void *)(uintptr_t)address;
    }
#ifdef _WIN32
    if (address < MEMORIES_GUEST_RAM_SIZE && !low_piece_mapped[address >> 16]) {
        return (void *)(uintptr_t)address;
    }
#endif
    if ((target = guest_call_target(address)) != NULL) {
        return target;
    }
    report_guest_fault(address, address);
    snprintf(text, sizeof(text), "0x%08x has no native function", address);
    Crash_ReportFatal("call into guest code", text);
    Profile_Flush();
    _exit(70);
}

/* The thunks let a host target through after one test of its address
 * bits (branch_thunks.c). The executable has a fixed base where that test
 * passes; were its code somewhere the test fails, calls would still work,
 * through the resolver, only slower: say so. */
static void check_code_address(void)
{
    if (!((uintptr_t)check_code_address & 0x5fe00000u)) {
        fprintf(stderr, "memories-pc: the executable's code (0x%08x) is where the branch thunks take the slow path\n",
                (unsigned)(uintptr_t)check_code_address);
    }
}

/* Guest RAM mapped executable (MEMORIES_TEST_EXEC_GUEST=1), as it is where
 * DEP is off: then only the thunks keep a guest call from running MIPS bytes,
 * which makes "the game works without DEP" testable on any machine. Only
 * builds that are not releases have it (MEMORIES_TEST_HOOKS, set by
 * tools/pc/build_game32.py without --release): a shipped executable that
 * can map memory writable and executable is one more thing virus scanners'
 * heuristics hold against it. */
#ifdef MEMORIES_TEST_HOOKS
static int guest_ram_executable(void)
{
    const char *value = getenv("MEMORIES_TEST_EXEC_GUEST");
    if (!value || !*value || !strcmp(value, "0")) return 0;
    fprintf(stderr, "memories-pc: guest RAM is mapped executable (MEMORIES_TEST_EXEC_GUEST)\n");
    return 1;
}
#endif

#ifdef _WIN32
static DWORD *context_register(CONTEXT *context, int number)
{
    switch (number) {
    case 0: return &context->Eax;
    case 1: return &context->Ecx;
    case 2: return &context->Edx;
    case 3: return &context->Ebx;
    case 4: return &context->Esp;
    case 5: return &context->Ebp;
    case 6: return &context->Esi;
    default: return &context->Edi;
    }
}

/* The common low access, a plain 32-bit MOV to or from [base + disp], is
 * done here through guest RAM instead of the rebase-and-single-step path:
 * 32-bit processes on 64-bit Windows can mishandle the trap when the
 * instruction's destination is its own base register. Returns 1 if done. */
static int emulate_low_mov(CONTEXT *context, uint32_t address)
{
    const unsigned char *code = (const unsigned char *)(uintptr_t)context->Eip;
    unsigned modrm, mod, reg, rm;
    uint32_t *guest;
    if ((code[0] != 0x8b && code[0] != 0x89) || address > MEMORIES_GUEST_RAM_SIZE - 4) {
        return 0;
    }
    modrm = code[1];
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    if (mod == 3 || rm == 4 || (mod == 0 && rm == 5)) {
        return 0; /* register operand, SIB byte or absolute address */
    }
    guest = (uint32_t *)(uintptr_t)(MEMORIES_GUEST_RAM + address);
    if (code[0] == 0x8b) {
        *context_register(context, (int)reg) = *guest;
    } else {
        *guest = *context_register(context, (int)reg);
    }
    context->Eip += 2u + (mod == 1 ? 1u : mod == 2 ? 4u : 0u);
    return 1;
}

/* First in line for every exception in the process: take the guest's own
 * faults, leave everything else to the next handler (win32.c reports what
 * the executable raised). */
static LONG CALLBACK on_guest_exception(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    uint32_t address;
    void *target;
    Win32_UndoInterruptedFault(context); /* then handled as the faulting instruction's own */
    /* A 32-bit process on 64-bit Windows may see the trap as WoW64's own
     * STATUS_WX86_SINGLE_STEP. */
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP || record->ExceptionCode == 0x4000001eu) {
        DWORD *reg;
        if (!low_fixup.active) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        low_fixup.active = 0;
        reg = context_register(context, low_fixup.reg);
        if (*reg == low_fixup.patched) {
            *reg = low_fixup.original;
        }
        context->EFlags &= ~0x100; /* trap flag */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uint32_t)record->ExceptionInformation[1];
    /* The first 64 KiB and the parts of the physical mirror Windows holds
     * (see Memories_GuestMap) are reached the same way: through guest RAM
     * at 0x80000000, which the same offsets address. */
    if (address < MEMORIES_GUEST_RAM_SIZE && context->Eip != address && !low_fixup.active) {
        int reg = low_access_register((const unsigned char *)(uintptr_t)context->Eip, context->Esi);
        if (reg >= 0 && *context_register(context, reg) < MEMORIES_GUEST_RAM_SIZE) {
            report_low_access(context->Eip, address);
            if (emulate_low_mov(context, address)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = *context_register(context, reg);
            low_fixup.patched = low_fixup.original + MEMORIES_GUEST_RAM;
            *context_register(context, reg) = low_fixup.patched;
            context->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (context->Eip == address && (target = guest_call_target(address)) != NULL) {
        context->Eip = (DWORD)(uintptr_t)target;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (context->Eip == address || address < 0x10000u ||
        (address >= MEMORIES_GUEST_RAM && address < MEMORIES_GUEST_RAM + 0x00800000u)) {
        report_guest_fault(address, context->Eip);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The Linux layout as far as Windows allows: one pagefile-backed section
 * holds guest RAM and is viewed at 0x80000000 and 0xA0000000. The physical
 * mirror (0x10000..0x200000) competes with what Windows puts there before
 * the program starts (process parameters, locale tables, the WoW64 stack),
 * so it is mapped in 64 KiB pieces where the address space is free; an
 * access to a piece Windows holds faults and goes through guest RAM
 * instead (on_guest_exception). Such accesses into pages Windows has
 * mapped readable would not fault; the sites that fault are reported. */
static DWORD view_access = FILE_MAP_ALL_ACCESS;

static int view_at(HANDLE section, uint32_t address, size_t length, DWORD offset)
{
    void *wanted = (void *)(uintptr_t)address;
    if (MapViewOfFileEx(section, view_access, 0, offset, length, wanted) != wanted) {
        fprintf(stderr, "cannot map guest memory at 0x%08x (error %lu)\n", (unsigned)address,
                GetLastError());
        return -1;
    }
    return 0;
}

/* Calls into guest code go through the branch thunks: the game works
 * without DEP. Where DEP is on, guest RAM mapped without execute permission
 * is a second safety net: a call that escaped the thunks faults into
 * on_guest_exception instead of running MIPS bytes. The game is a 32-bit
 * process, which follows the system's DEP policy (only 64-bit processes
 * always have DEP): under OptIn, the default, the executable's --nxcompat
 * turns it on. The game does not change the policy itself: a program that
 * calls SetProcessDEPPolicy is what virus scanners' heuristics look for. */
int Memories_GuestMap(void)
{
    HANDLE section;
    DWORD protection = PAGE_READWRITE;
    int result;
#ifdef MEMORIES_TEST_HOOKS
    if (guest_ram_executable()) {
        protection = PAGE_EXECUTE_READWRITE;
        view_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
    }
#endif
    Memories_GuestBranchResolver = guest_branch_target;
    check_code_address();
    section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, protection, 0, MEMORIES_GUEST_RAM_SIZE, NULL);
    AddVectoredExceptionHandler(1, on_guest_exception);
    if (section == NULL) {
        fprintf(stderr, "guest RAM: CreateFileMapping failed (error %lu)\n", GetLastError());
        return -1;
    }
    /* The mirror first: tested on Windows 11, a view below 0x200000 fails
     * with ERROR_INVALID_ADDRESS once the high views exist. */
    {
        uint32_t piece, held = 0;
        for (piece = 0x10000u; piece < MEMORIES_GUEST_RAM_SIZE; piece += 0x10000u) {
            if (MapViewOfFileEx(section, view_access, 0, piece, 0x10000u, (void *)(uintptr_t)piece) == NULL) {
                held += 0x10000u;
            } else {
                low_piece_mapped[piece >> 16] = 1;
            }
        }
        if (held) {
            fprintf(stderr, "memories-pc: %u KiB of the physical RAM mirror are taken by Windows; accesses there fault into guest RAM\n",
                    (unsigned)(held / 1024));
        }
    }
    result = view_at(section, MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, 0) ||
             view_at(section, 0xa0000000u, MEMORIES_GUEST_RAM_SIZE, 0);
    if (!result && VirtualAlloc((void *)0x1f800000u, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE) != (void *)0x1f800000u) {
        fprintf(stderr, "cannot map the scratchpad at 0x1f800000 (error %lu)\n", GetLastError());
        result = -1;
    }
    /* The views keep the section alive. */
    CloseHandle(section);
    return result ? -1 : 0;
}
#else
static int view_protection = PROT_READ | PROT_WRITE;

static int map_at(uint32_t address, size_t length, int fd, off_t offset)
{
    void *wanted = (void *)(uintptr_t)address;
    int flags = MAP_FIXED_NOREPLACE | (fd < 0 ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_SHARED);
    if (mmap(wanted, length, fd < 0 ? PROT_READ | PROT_WRITE : view_protection, flags, fd, offset) != wanted) {
        fprintf(stderr, "cannot map guest memory at 0x%08x\n", (unsigned)address);
        return -1;
    }
    return 0;
}

#if defined(__i386__)
/* ModRM/SIB register numbers to gregs[]. */
static const int register_slot[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};

static void on_step(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    greg_t *reg;
    (void)number; (void)info;
    if (low_fixup.active) {
        low_fixup.active = 0;
        reg = &user->uc_mcontext.gregs[register_slot[low_fixup.reg]];
        if ((uint32_t)*reg == low_fixup.patched) {
            *reg = (greg_t)low_fixup.original;
        }
    } else {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigaction(SIGTRAP, &action, NULL);
        raise(SIGTRAP);
        return;
    }
    user->uc_mcontext.gregs[REG_EFL] &= ~0x100; /* trap flag */
}

static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    uint32_t eip = (uint32_t)user->uc_mcontext.gregs[REG_EIP];
    void *target;
    if (address < 0x10000u && eip != address && low_memory && !low_fixup.active) {
        int reg = low_access_register((const unsigned char *)(uintptr_t)eip, (uint32_t)user->uc_mcontext.gregs[REG_ESI]);
        if (reg >= 0 && (uint32_t)user->uc_mcontext.gregs[register_slot[reg]] < 0x10000u) {
            report_low_access(eip, address);
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = (uint32_t)user->uc_mcontext.gregs[register_slot[reg]];
            low_fixup.patched = low_fixup.original + (uint32_t)(uintptr_t)low_memory;
            user->uc_mcontext.gregs[register_slot[reg]] = (greg_t)low_fixup.patched;
            user->uc_mcontext.gregs[REG_EFL] |= 0x100;
            return;
        }
    }
    if (eip == address && (target = guest_call_target(address)) != NULL) {
        user->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)target;
        return;
    }
    report_guest_fault(address, eip);
    Crash_HandleSignal(number, info, context);
}

#elif defined(__arm__)
/* 32-bit ARM (Android) has no indirect-branch thunks -- the compiler offers
 * nothing like -mindirect-branch=thunk-extern there (branch_thunks.c) -- so
 * this is the only way into a native function from a pointer the retail data
 * image holds, and it carries the traffic the thunks take on x86. Guest RAM
 * is mapped without PROT_EXEC, so the call faults on its first instruction
 * and lands here.
 *
 * The redirect is exact on ARM in a way it is not on x86: AAPCS leaves the
 * return address in lr, which the faulting call already set, so moving pc to
 * the native function gives it the frame a direct call would. Nothing is
 * pushed, popped or realigned.
 *
 * An access below 0x10000 is repaired differently from the i386 handler's
 * way. That one points the instruction's base register at `low_memory` and
 * single-steps it with the trap flag; ARM has no trap flag, so the one
 * faulting load or store is performed here instead and pc stepped past it.
 * It needs no second signal and leaves no window in which the register holds
 * the patched value. Only A32 encodings are decoded, which is all the build
 * produces (-marm). */

/* The registers of the faulting context: struct sigcontext holds r0-r10,
 * fp, ip, sp, lr and pc as consecutive words, which is the order they
 * number in. */
static unsigned long *context_registers(ucontext_t *user)
{
    return &user->uc_mcontext.arm_r0;
}

/* Perform the faulting A32 load or store against `low_memory` and step over
 * it. si_addr is the effective address the instruction computed, so only the
 * transfer itself has to be decoded -- and the offset only when the
 * instruction writes the base register back. Returns 0 for an encoding this
 * does not cover, which is then reported as a fault. */
static int emulate_low_access(ucontext_t *user, uint32_t address)
{
    unsigned long *reg = context_registers(user);
    uint32_t code = *(const uint32_t *)(uintptr_t)user->uc_mcontext.arm_pc;
    unsigned char *at = low_memory + address;
    unsigned rt = (code >> 12) & 0xfu, rn = (code >> 16) & 0xfu;
    unsigned load = (code >> 20) & 1u, pre = (code >> 24) & 1u, up = (code >> 23) & 1u;
    unsigned writeback = pre ? (code >> 21) & 1u : 1u;
    unsigned size, sign_extend = 0;
    uint32_t offset = 0;
    if (rt == 15u || rn == 15u) {
        return 0; /* pc as the data or the base: not one of these sites */
    }
    if ((code & 0x0c000000u) == 0x04000000u) { /* LDR, LDRB, STR, STRB */
        size = (code & (1u << 22)) ? 1u : 4u;
        if (code & (1u << 25)) { /* scaled register offset */
            if (writeback && (code & 0xff0u) != 0) {
                return 0; /* a shifted offset written back: not decoded */
            }
            offset = (uint32_t)reg[code & 0xfu];
        } else {
            offset = code & 0xfffu;
        }
    } else if ((code & 0x0e000090u) == 0x00000090u && (code & 0x60u) != 0) {
        /* LDRH, STRH, LDRSB, LDRSH: the offset is split either side of the
         * shape bits, and bit 22 picks immediate over register. */
        unsigned shape = (code >> 5) & 3u;
        size = (shape == 1u) ? 2u : ((code & (1u << 20)) && shape == 2u ? 1u : 2u);
        sign_extend = load && shape != 1u;
        if (code & (1u << 22)) {
            offset = ((code >> 4) & 0xf0u) | (code & 0xfu);
        } else {
            offset = (uint32_t)reg[code & 0xfu];
        }
    } else {
        return 0; /* not a single load or store: block transfer, VFP, swap */
    }
    if (load) {
        uint32_t value = 0;
        memcpy(&value, at, size);
        if (sign_extend && size == 1u) {
            value = (uint32_t)(int32_t)(int8_t)value;
        } else if (sign_extend && size == 2u) {
            value = (uint32_t)(int32_t)(int16_t)value;
        }
        reg[rt] = value;
    } else {
        uint32_t value = (uint32_t)reg[rt];
        memcpy(at, &value, size);
    }
    if (writeback) {
        reg[rn] = pre ? address : (up ? address + offset : address - offset);
    }
    user->uc_mcontext.arm_pc += 4;
    return 1;
}
static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    uint32_t pc = (uint32_t)user->uc_mcontext.arm_pc;
    void *target;
    if (pc == address && (target = guest_call_target(address)) != NULL) {
        user->uc_mcontext.arm_pc = (unsigned long)(uintptr_t)target;
        return;
    }
    if (address < 0x10000u && pc != address && low_memory) {
        report_low_access(pc, address);
        if (emulate_low_access(user, address)) {
            return;
        }
    }
    report_guest_fault(address, pc);
    Crash_HandleSignal(number, info, context);
}
#elif defined(__aarch64__)
/* The AArch64 handler, which is the 32-bit ARM one with A64's register names
 * and encodings. The same two jobs, for the same reasons (see above): no
 * compiler here offers an indirect-branch thunk either, so guest RAM is
 * mapped without PROT_EXEC and a call through a pointer the retail data
 * image holds faults and is redirected; and an access below 0x10000 is
 * performed here rather than single-stepped, there being no trap flag. */
static void on_fault(int number, siginfo_t *info, void *context);

/* Perform the faulting A64 load or store against `low_memory` and step over
 * it. si_addr is the effective address, so only the transfer is decoded --
 * and the offset only for the forms that write the base register back.
 * Returns 0 for an encoding this does not cover. */
static int emulate_low_access(ucontext_t *user, uintptr_t address)
{
    /* The context's own type: __u64 is unsigned long long, which is not
     * uint64_t on LP64 though it is the same width. */
    unsigned long long *reg = user->uc_mcontext.regs; /* x0 to x30 */
    uint32_t code = *(const uint32_t *)(uintptr_t)user->uc_mcontext.pc;
    unsigned char *at = low_memory + address;
    unsigned size = code >> 30, opc = (code >> 22) & 3u;
    unsigned rt = code & 0x1fu, rn = (code >> 5) & 0x1fu;
    unsigned kind = (code >> 24) & 3u, load, bytes, writeback = 0, pre = 0;
    int64_t offset = 0;
    uint64_t value;
    if (((code >> 27) & 7u) != 7u || (code & (1u << 26))) {
        return 0; /* not a load or store of a general register (26 is SIMD) */
    }
    if (opc == 0) {
        load = 0;
    } else if (size == 3u && opc == 2u) {
        return 0; /* PRFM, which touches nothing */
    } else {
        load = 1;
    }
    bytes = 1u << size;
    if (kind == 1u) {
        /* unsigned immediate offset: no writeback */
    } else if (kind == 0u) {
        unsigned form = (code >> 10) & 3u;
        if (code & (1u << 21)) {
            if (form != 2u) {
                return 0; /* not the register-offset form */
            }
        } else if (form == 1u || form == 3u) {
            /* post- and pre-indexed, which write the base back */
            writeback = 1;
            pre = form == 3u;
            offset = (int64_t)((int32_t)(code << 11) >> 23); /* signed imm9 */
        } else if (form != 0u) {
            return 0; /* not the unscaled form either */
        }
    } else {
        return 0; /* a pair, or an atomic */
    }
    if (load) {
        value = 0;
        memcpy(&value, at, bytes);
        if (opc >= 2u) { /* sign-extended to 64 bits, or to 32 */
            unsigned width = bytes * 8u;
            value = (uint64_t)(((int64_t)(value << (64u - width))) >> (64u - width));
            if (opc == 3u) {
                value = (uint32_t)value;
            }
        } else if (size != 3u) {
            value = (uint32_t)value; /* a 32-bit load zeroes the top half */
        }
        if (rt != 31u) { /* 31 is the zero register, not sp, for a transfer */
            reg[rt] = value;
        }
    } else {
        value = rt == 31u ? 0 : reg[rt];
        memcpy(at, &value, bytes);
    }
    if (writeback) {
        uint64_t written = pre ? (uint64_t)address : (uint64_t)address + (uint64_t)offset;
        if (rn == 31u) {
            user->uc_mcontext.sp = written;
        } else {
            reg[rn] = written;
        }
    }
    user->uc_mcontext.pc += 4;
    return 1;
}

static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uintptr_t fault = (uintptr_t)info->si_addr;
    uint64_t pc = user->uc_mcontext.pc;
    void *target;
    if (pc == (uint64_t)fault && (target = guest_call_target((uint32_t)fault)) != NULL) {
        /* AAPCS64 leaves the return address in x30, which the faulting call
         * already set, so moving pc is the whole redirect. */
        user->uc_mcontext.pc = (uint64_t)(uintptr_t)target;
        return;
    }
    if (fault < 0x10000u && pc != (uint64_t)fault && low_memory) {
        report_low_access((uint32_t)pc, (uint32_t)fault);
        if (emulate_low_access(user, fault)) {
            return;
        }
    }
    report_guest_fault((uint32_t)fault, (uint32_t)pc);
    Crash_HandleSignal(number, info, context);
}
#else
#error "no guest fault handler for this architecture"
#endif

/* The shared file the guest RAM mirrors are views of. bionic only declares
 * memfd_create from API 30 and this build targets older Android, so the
 * syscall is made directly; the kernel has had it since 3.17, well below
 * anything that runs this. */
static int memories_ram_fd(void)
{
#if defined(__ANDROID__)
    return (int)syscall(__NR_memfd_create, "memories-ram", 0u);
#else
    return memfd_create("memories-ram", 0);
#endif
}

int Memories_GuestMap(void)
{
    struct sigaction action;
    int fd, result;
#ifdef MEMORIES_TEST_HOOKS
    if (guest_ram_executable()) view_protection |= PROT_EXEC;
#endif
    Memories_GuestBranchResolver = guest_branch_target;
    check_code_address();
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
#if defined(__i386__)
    action.sa_sigaction = on_step;
    sigaction(SIGTRAP, &action, NULL);
#endif
    fd = memories_ram_fd();
    if (fd < 0 || ftruncate(fd, MEMORIES_GUEST_RAM_SIZE) != 0) {
        perror("guest RAM");
        return -1;
    }
    result = map_at(MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0xa0000000u, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0x00010000u, MEMORIES_GUEST_RAM_SIZE - 0x10000u, fd, 0x10000) ||
             map_at(0x1f800000u, 0x1000, -1, 0);
    low_memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (low_memory == MAP_FAILED) {
        low_memory = NULL;
    }
    close(fd);
    return result ? -1 : 0;
}
#endif /* _WIN32 */

static uint32_t le32(const unsigned char *bytes)
{
    return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

int Memories_GuestLoadExeData(const unsigned char *data, size_t length, const char *name)
{
    uint32_t address, size;
    if (length < 0x800 || memcmp(data, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a readable PS-X executable\n", name);
        return -1;
    }
    address = le32(data + 0x18);
    size = le32(data + 0x1c);
    if (address < MEMORIES_GUEST_RAM + 0x10000u || size > MEMORIES_GUEST_RAM_SIZE ||
        address - MEMORIES_GUEST_RAM > MEMORIES_GUEST_RAM_SIZE - size || size > length - 0x800) {
        fprintf(stderr, "%s: image does not fit guest RAM or is truncated\n", name);
        return -1;
    }
    memcpy((void *)(uintptr_t)address, data + 0x800, size);
    return 0;
}

int Memories_GuestLoadExe(const char *path)
{
    unsigned char *data;
    long length;
    int result;
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) ||
        !(data = malloc(length ? (size_t)length : 1))) {
        fprintf(stderr, "%s: not a readable PS-X executable\n", path);
        if (file) fclose(file);
        return -1;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) length = 0;
    fclose(file);
    result = Memories_GuestLoadExeData(data, (size_t)length, path);
    free(data);
    return result;
}

typedef struct StubCount { const char *name; unsigned count; } StubCount;
static StubCount stub_calls[512];
static unsigned stub_call_count;

static int compare_stub_counts(const void *left, const void *right)
{
    const StubCount *a = left, *b = right;
    return a->count < b->count ? 1 : a->count > b->count ? -1 : strcmp(a->name, b->name);
}

static void print_stub_summary(void)
{
    unsigned at;
    qsort(stub_calls, stub_call_count, sizeof(stub_calls[0]), compare_stub_counts);
    for (at = 0; at < stub_call_count; at++) {
        LOG(LOG_STUB, "%s: %u calls", stub_calls[at].name, stub_calls[at].count);
    }
    Log_Drain();
}

void Memories_Unimplemented(const char *name)
{
    static int registered;
    const char *break_name = getenv("MEMORIES_STUB_BREAK");
    unsigned i;
#ifdef _WIN32
    if (break_name && !strcmp(break_name, name)) DebugBreak();
#else
    if (break_name && !strcmp(break_name, name)) raise(SIGTRAP);
#endif
    /* Survey aid only: results after the first line are not meaningful,
     * because the missing routine returned garbage. */
    if (getenv("MEMORIES_STUB_TRACE")) {
        for (i = 0; i < stub_call_count && strcmp(stub_calls[i].name, name); i++) {}
        if (i == stub_call_count && stub_call_count < sizeof(stub_calls) / sizeof(stub_calls[0])) {
            stub_calls[stub_call_count].name = name;
            stub_calls[stub_call_count++].count = 0;
        }
        if (i < stub_call_count) stub_calls[i].count++;
        if (!registered) {
            registered = 1;
            Log_Enable(LOG_STUB, 1);
            atexit(print_stub_summary);
        }
        LOG(LOG_STUB, "%s", name);
        return;
    }
    fflush(stdout);
    Crash_ReportFatal("unimplemented routine", name);
    Profile_Flush();
    _exit(70); /* not exit(): atexit handlers could re-enter game code */
}
