#ifndef MEMORIES_PC_COMPAT_GUEST_FN_H
#define MEMORIES_PC_COMPAT_GUEST_FN_H
/* A function pointer the game keeps in guest memory.
 *
 * A slot the game stores a pointer in is four bytes wide, on the console and
 * in every build of the port (src/port_ptr.h). In an ILP32 build a native
 * function's address is four bytes too, so the port can put one there and
 * the game calls it directly. A 64-bit build cannot: the library is placed
 * where the loader likes, and no four-byte slot can hold that address. The
 * link says so rather than guess -- "R_AARCH64_ABS32 cannot be used against
 * symbol ...".
 *
 * So a 64-bit build stores the retail guest address instead, which is what
 * the retail image holds in the first place. A call through it lands in
 * guest RAM, which is mapped without PROT_EXEC, and the fault handler sends
 * it to the native function (src/pc/guest/branch_thunks.c, image.c) -- the
 * same path every other table of MIPS addresses in guest memory takes. The
 * call does the same thing either way; only what the four bytes spell
 * changes.
 *
 * GUEST_FN(type, name, address) is the function `name`, whose retail address
 * is `address`, as `type`. Wherever MEMORIES_PC is not defined it is the
 * name alone, so the console's build and the matching build are unchanged.
 */
#include "types.h"

#if defined(MEMORIES_PC) && defined(__LP64__)
/* Only this branch needs it, so the matching build's tokens stay as they
 * were: the game's own types.h does not carry uintptr_t. */
#include <stdint.h>
#define GUEST_FN(type, name, address) ((type)(uintptr_t)(address))
#else
#define GUEST_FN(type, name, address) ((type)name)
#endif

#endif
