/* What bionic does not define of the C library the game units call.
 *
 * The game's own code uses Psy-Q's libc, whose names the build takes from the
 * host's where the contract is the same (HOST_LIBC in
 * tools/pc/build_game32.py). bzero is the one glibc exports and bionic does
 * not: Android has only the <strings.h> macro, so a unit that calls it by
 * name -- src/game/ai_script_vm.c does -- leaves an undefined symbol.
 * Defining it here keeps the game source as the console had it. */
#ifdef __ANDROID__
#include <string.h>

void bzero(void *destination, size_t length)
{
    memset(destination, 0, length);
}
#else
/* Not this system's: the translation unit stays, empty but for this. */
typedef int memories_android_libc_unused;
#endif
