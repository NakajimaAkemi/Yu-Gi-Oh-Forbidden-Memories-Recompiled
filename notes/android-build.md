# Android build

The port as an installable APK: one shared library holding the whole game,
loaded by an activity that is SDL3's with a little added
(`android/MemoriesActivity.java`). No game data is in it; the player brings
their own disc image, as on the desktop.

```sh
python3 tools/pc/build_android_deps.py          # the NDK, the SDK, SDL3, FreeType, libpng
python3 tools/pc/build_game32.py --target android
python3 tools/pc/build_apk.py                  # tmp/pc/android/memories.apk
adb install -r tmp/pc/android/memories.apk
```

The first command needs about 4 GB under `tmp/pc/android-deps` and only runs
once; `ANDROID_NDK` and `ANDROID_HOME` are used when they already name an
installation. The build is the same kind of thing as the other two: Python
drivers calling the SDK's own tools, no Gradle and no project files to keep in
step.

## Why 32-bit ARM

The port's memory model is ILP32 (`src/pc/guest/image.h`): the 2 MiB of PS1
RAM is mapped at its retail KSEG0 address, game globals are linked at their
retail addresses, and the structures the game stores hold 32-bit pointers.
`armeabi-v7a` gives all of that natively, so the model carries over with no
pointer work at all.

`arm64-v8a` does not, and cannot without a different model.
`src/port_ptr.h` was written expecting clang's `__ptr32 __uptr` to give a
64-bit AArch64 build 4-byte pointers inside the game's structures, as it does
on x86-64. It does not: clang implements `__ptr32` through the x86 address
spaces, and on AArch64 it **parses and is ignored** with no "attribute
ignored" warning, only the generic `-Wlanguage-extension-token` note. Every
one of the 831 `G32` annotations would silently widen to 8 bytes and every
guest structure's layout would be wrong. A test worth keeping in mind:

```c
struct S { unsigned char *__ptr32 __uptr p; };
_Static_assert(sizeof(struct S) == 4, "");   /* x86-64: passes. AArch64: fails at 8 */
```

So a 64-bit Android port needs the guest pointers made explicit in the source,
not an attribute, and that is a much larger piece of work than this one.
The cost of shipping `armeabi-v7a` alone is that devices with no 32-bit
support -- Pixel 7 and later, and some other recent flagships -- cannot run
it.

## What the ARM port needed

Three things the x86 build does with machine-specific code:

**The guest glue.** `src/pc/guest/setjmp_arm.S` and `state_arm.S` are the
AAPCS counterparts of the i386 pair: ten words (`r4`-`r11`, `sp`, `lr`) where
i386 keeps five, since the return address is in a register rather than on the
stack. `MemoriesStateEntry` carries both shapes. `Memories_ContextSwitch` is
there too, because bionic has no `getcontext`, `makecontext` or `swapcontext`
-- Android never shipped them -- so the game reaches its fixed stack the way
the Windows port does.

**Calls into guest addresses.** The retail data image holds MIPS function
addresses, and native code calls through them. On x86 every unit's indirect
calls go through `__x86_indirect_thunk_<reg>`
(`-mindirect-branch=thunk-extern`), which 32-bit ARM has no counterpart of.
It does not need one: guest RAM is mapped without `PROT_EXEC`, so such a call
faults on its first instruction and `on_fault` sends it to the native
function. That was the i386 port's "second net", and on ARM it is exact in a
way it is not on x86 -- AAPCS leaves the return address in `lr`, which the
faulting call already set, so moving `pc` gives the callee the frame a direct
call would. Nothing is pushed, popped or realigned.

**An access below 0x10000.** Retail code reaches the console's kernel RAM
through null pointers (`CardList_CreateSlotTextBox` writes through one), and
no host lets a process map page zero. The i386 handler points the
instruction's base register at a spare mapping and single-steps it with the
trap flag; ARM has no trap flag, so `emulate_low_access` performs the one
faulting load or store itself and steps `pc` past it. Only A32 encodings are
decoded, which is all the build produces: it compiles `-marm` throughout,
which also lets the guest glue reach its own globals with `add rN, pc, rN`
and spares every generated stub an interworking veneer.

## Pinning retail addresses inside a shared library

An APK loads a library, and Android places it where it likes, so the 783
pinned retail addresses have to survive relocation. They do, and the recipe
matters:

  * the pins are a linker script of **plain assignments** (`sym = 0x80012344;`)
    passed as an input file. `PROVIDE_HIDDEN` does not work: it defines only
    an undefined symbol, and most pinned names are a game unit's tentative
    (COMMON) definition, which a plain assignment overrides and `PROVIDE` does
    not;
  * a **version script** keeps everything but `SDL_main` local. That is what
    makes it correct: a local absolute symbol is folded into the GOT at link
    time, where a global one leaves an `R_ARM_GLOB_DAT` for the loader to
    resolve by symbol lookup -- and bionic resolves a symbol by adding the
    library's load bias, which would move every retail address;
  * `-fvisibility=hidden` throughout, which also lets the assembly reach its
    globals at a link-time offset rather than through a text relocation.
    Android rejects those outright.

The result has no text relocations and no dynamic relocation for any pinned
symbol; `llvm-nm` shows each as a lowercase `a` at its retail address.

## What is not the same as the desktop build

| | Why |
|---|---|
| SDL_Render, not OpenGL | The presenter is written against the desktop fixed-function pipeline -- `glBegin`, `glMatrixMode`, `glOrtho` -- which GLES has never had. `src/pc/render/gl_none.c` stands in for it and `use_gl` stays 0. A GLES presenter is self-contained follow-up work: the picture is one textured quad and the effects are fragment shaders. |
| No save states across rebuilds | A shared library's sections cannot be placed at chosen addresses, exactly as a PE's cannot. States within one build work. |
| Data mods only | A mod's code is an object the game links at load, so it must be the game's own machine code, and `tools/pc/build_mod.py` only builds x86. Nothing would hook anyway: clang has no `-fpatchable-function-entry` for 32-bit ARM, so the library has no patch room (`src/pc/mods/hooks.c`). |
| No crash monitor | The monitor is a second process watching the game with ptrace. An app is one process with no ptrace over another, so the game runs alone -- what `MEMORIES_NO_MONITOR` asks for elsewhere. The facts and the log tail still reach its reports. |
| No update check | There is no `curl` to spawn, and an app does not replace itself. The APK is how a new version arrives. |
| Fonts from `/system/fonts` | No fontconfig. `src/pc/platform/android_fonts.c` names the Noto and Droid faces. |

## The player's folder

One directory holds everything:
`/sdcard/Android/data/com.yfm.redecomp/files`, which any file manager reaches
and which needs no storage permission. The activity unpacks the mods and the
languages into it on first start and names it to the game through
`MEMORIES_PROGRAM_DIR` and `MEMORIES_USER_DIR`, because an app has no command
line and `/proc/self/exe` is the runtime that loaded the library rather than a
program directory. The disc image goes in `game/` inside it, where a note says
so; saves, save states, screenshots and settings appear beside it.

## On-screen pad

`src/pc/platform/touch_pad.c` draws a controller over the game and produces
the same PS1 pad bits a keyboard or a real pad does, so nothing downstream
knows the difference. Two clusters at the bottom corners, the console's own
symbols on the face buttons, the shoulders above each cluster and Select and
Start between them. Diagonals come from the directions being a three-by-three
grid whose four corner cells are invisible and hold two bits each.

It is painted into the overlay canvas the menu and the HUD share, in software,
as distance fields -- which needs no art, no font for the symbols and no extra
texture -- and only when something changes, so it costs nothing per frame. A
connected controller hides it.

## Not yet run

The library links, the APK installs and the whole path is reproducible, but
**the game has not been played on a device**: that needs a disc image and
hardware, and an `armeabi-v7a` image cannot be emulated on an x86-64 host
(the Android emulator dropped ARM guests). What to watch on a first run:

```sh
adb logcat -s memories:V SDL:V DEBUG:V AndroidRuntime:E
```

The first thing to confirm is the memory map, which is the one part of the
model that no amount of linking proves. `Memories_GuestMap` wants 2 MiB at
`0x80000000`, mirrors at `0xA0000000` and `0x00010000`, the scratchpad at
`0x1F800000` and an 8 MiB game stack at `0x70000000`, each with
`MAP_FIXED_NOREPLACE`; it prints `cannot map guest memory at 0x...` if
something is already there. A 32-bit process on a 64-bit kernel gets the whole
4 GB of address space, so there is room, but nothing has checked that
Android's own loader leaves those ranges free. `MAP_FIXED_NOREPLACE` also
needs Linux 4.17, which is below any device this installs on, and degrades to
a hint rather than silently clobbering on anything older.
