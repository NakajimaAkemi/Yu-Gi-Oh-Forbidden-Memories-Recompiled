#!/usr/bin/env python3
"""Fetch and build what the Android game library needs.

The Linux build takes its libraries from Debian 11 and the Windows one from
llvm-mingw (tools/pc/build_linux_sysroot.py, tools/pc/build_win32_deps.py).
This is the same thing for Android, under tmp/pc/android-deps:

  * the NDK, which is the compiler (ndk/), and the SDK's platform, build
    tools and cmake, which tools/pc/build_apk.py packages with (sdk/). Both
    serve every ABI;
  * per ABI, under <abi>/: SDL3 and its Java glue (sdl/), and static libpng
    and FreeType (include/, lib/). --abi picks which; the APK carries one
    library per ABI it supports, so both are built in turn.

The libpng and FreeType archives are the ones the Windows build pins, so the
three platforms link the same versions. zlib is not among them: Android ships
one, as it ships GLES and the log library.

ANDROID_NDK and ANDROID_HOME are used when they already name an installation,
so a machine with the SDK set up downloads neither. Nothing needs installing
by hand.

API and ABI must stay what tools/pc/build_game32.py compiles for.
"""
import argparse, hashlib, os, shutil, stat, subprocess, sys, tarfile, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tmp", "pc", "android-deps")
DOWNLOADS = os.path.join(OUT, "downloads")
ABIS = ("armeabi-v7a", "arm64-v8a")
API = 28
NDK_VERSION = "r27c"
BUILD_TOOLS = "34.0.0"
PLATFORM = "android-34"
CMAKE_VERSION = "3.22.1"
LLVM_VERSION = "llvmorg-19.1.7"
LLVM_PATCH = os.path.join("tools", "pc", "android", "llvm-aarch64-ptr32.patch")
ARCHIVES = {
    "llvm": (f"https://github.com/llvm/llvm-project/archive/refs/tags/{LLVM_VERSION}.tar.gz", None),
    "ndk": (f"https://dl.google.com/android/repository/android-ndk-{NDK_VERSION}-linux.zip",
            "59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc"),
    "cmdline-tools": ("https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip",
                      "2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258"),
    "sdl": ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz",
            "7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68"),
    "libpng": ("https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz",
               "a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf"),
    "freetype": ("https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz",
                 "dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261"),
}


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if result.returncode:
        sys.exit(f"{command[0]} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def fetch(name):
    """The extracted directory of a pinned archive, downloaded once."""
    url, digest = ARCHIVES[name]
    os.makedirs(DOWNLOADS, exist_ok=True)
    archive = os.path.join(DOWNLOADS, os.path.basename(url))
    have = os.path.exists(archive) and (digest is None or
                                        hashlib.sha256(open(archive, "rb").read()).hexdigest() == digest)
    if not have:
        print(f"fetching {url}")
        urllib.request.urlretrieve(url, archive)
        got = hashlib.sha256(open(archive, "rb").read()).hexdigest()
        if digest is not None and got != digest:
            sys.exit(f"{url}: expected sha256 {digest}, got {got}")
    out = os.path.join(DOWNLOADS, name + "-source")
    if not os.path.exists(out):
        temporary = out + ".part"
        shutil.rmtree(temporary, ignore_errors=True)
        os.makedirs(temporary)
        if archive.endswith(".zip"):
            with zipfile.ZipFile(archive) as zipped:
                # extractall drops what the NDK needs kept: the execute bit on
                # its compilers, and its symbolic links, which it would write
                # as ordinary files holding the target's name (clang is a link
                # to clang-<version>, so the compiler would not run).
                for entry in zipped.infolist():
                    target = os.path.join(temporary, entry.filename)
                    mode = entry.external_attr >> 16
                    if stat.S_ISLNK(mode):
                        os.makedirs(os.path.dirname(target), exist_ok=True)
                        if os.path.lexists(target):
                            os.remove(target)
                        os.symlink(zipped.read(entry).decode(), target)
                        continue
                    zipped.extract(entry, temporary)
                    if mode & 0o111 and not entry.is_dir():
                        os.chmod(target, mode & 0o777)
        else:
            with tarfile.open(archive) as tarred:
                # Python 3.14 filters by default and warns before then; the
                # argument is only there from 3.12.
                if hasattr(tarfile, "data_filter"):
                    tarred.extractall(temporary, filter="data")
                else:
                    tarred.extractall(temporary)
        inside = os.listdir(temporary)
        if len(inside) == 1 and os.path.isdir(os.path.join(temporary, inside[0])):
            os.rename(os.path.join(temporary, inside[0]), out)
            shutil.rmtree(temporary, ignore_errors=True)
        else:
            os.rename(temporary, out)
    return out


def ndk():
    """The NDK's root: the one ANDROID_NDK names, else a fetched copy."""
    named = os.environ.get("ANDROID_NDK")
    if named and os.path.isdir(named):
        return named
    out = os.path.join(OUT, "ndk")
    if not os.path.exists(out):
        os.makedirs(OUT, exist_ok=True)
        # Moved rather than copied: the NDK is some gigabytes, and a second
        # copy of it under downloads/ would serve no purpose.
        os.rename(fetch("ndk"), out)
    return out


def sdk():
    """The SDK's root, with the platform, build tools and cmake installed."""
    named = os.environ.get("ANDROID_HOME")
    root = named if named and os.path.isdir(named) else os.path.join(OUT, "sdk")
    wanted = [f"platforms;{PLATFORM}", f"build-tools;{BUILD_TOOLS}", f"cmake;{CMAKE_VERSION}"]
    have = all(os.path.exists(os.path.join(root, *path.split(";")).replace(";", os.sep))
               for path in ("platforms/" + PLATFORM, "build-tools/" + BUILD_TOOLS, "cmake/" + CMAKE_VERSION))
    if have:
        return root
    manager = os.path.join(root, "cmdline-tools", "bin", "sdkmanager")
    if not os.path.exists(manager):
        os.makedirs(root, exist_ok=True)
        shutil.copytree(fetch("cmdline-tools"), os.path.join(root, "cmdline-tools"), dirs_exist_ok=True)
    print(f"{root}: installing " + ", ".join(wanted))
    # The licences are accepted on the player's behalf for the packages this
    # build needs; they are Google's standard SDK terms.
    subprocess.run([manager, f"--sdk_root={root}", "--licenses"], input="y\n" * 64,
                   text=True, capture_output=True)
    run([manager, f"--sdk_root={root}", *wanted])
    return root


def cmake_build(abi, name, source, out, *options):
    """Configure and install one library for `abi` with the NDK."""
    toolchain = os.path.join(ndk(), "build", "cmake", "android.toolchain.cmake")
    cmake = os.path.join(sdk(), "cmake", CMAKE_VERSION, "bin", "cmake")
    build = os.path.join(OUT, "build", abi, name)
    run([cmake, "-S", source, "-B", build, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={abi}",
         f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={out}", *options],
        env={**os.environ, "PATH": os.path.dirname(cmake) + os.pathsep + os.environ.get("PATH", "")})
    run([cmake, "--build", build, "--target", "install"])


def llvm():
    """Build the clang the arm64 game needs, with
    tools/pc/android/llvm-aarch64-ptr32.patch applied.

    A stock clang accepts __ptr32 on AArch64 and ignores it, so every guest
    structure would be laid out wrong (src/port_ptr.h, which stops such a
    build). The patch gives AArch64 the address spaces x86 has. Only the
    compiler is built: the NDK's ld.lld does the linking, and its compiler
    runtime and unwinder are linked into the resource directory below, so
    this clang needs nothing of its own at link time.

    Takes some tens of minutes and about 25 GB under tmp/pc/android-deps.
    """
    out = os.path.join(OUT, "llvm")
    if os.path.exists(os.path.join(out, "bin", "clang")):
        return out
    source = fetch("llvm")
    patch = os.path.join(ROOT, LLVM_PATCH)
    applied = os.path.join(source, ".ptr32-patched")
    if not os.path.exists(applied):
        print(f"applying {LLVM_PATCH}")
        run(["git", "apply", "--directory", ".", patch], cwd=source)
        open(applied, "w").close()
    cmake = os.path.join(sdk(), "cmake", CMAKE_VERSION, "bin", "cmake")
    build = os.path.join(OUT, "build", "llvm")
    print(f"building clang {LLVM_VERSION} (this takes a while)")
    run([cmake, "-S", os.path.join(source, "llvm"), "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_ENABLE_PROJECTS=clang",
         "-DLLVM_TARGETS_TO_BUILD=AArch64;ARM;X86", "-DLLVM_ENABLE_ASSERTIONS=OFF",
         "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
         "-DLLVM_INCLUDE_EXAMPLES=OFF", f"-DCMAKE_INSTALL_PREFIX={out}",
         "-DLLVM_PARALLEL_LINK_JOBS=4"],
        env={**os.environ, "PATH": os.path.dirname(cmake) + os.pathsep + os.environ.get("PATH", "")})
    run([cmake, "--build", build, "--target", "clang"])
    # install-clang would want the whole of LLVM installed; the driver and its
    # resource directory are all that is used.
    os.makedirs(os.path.join(out, "bin"), exist_ok=True)
    for name in sorted(os.listdir(os.path.join(build, "bin"))):
        if name.startswith("clang"):
            shutil.copy2(os.path.join(build, "bin", name), os.path.join(out, "bin", name),
                         follow_symlinks=False)
    resource = os.path.join(build, "lib", "clang")
    version = sorted(os.listdir(resource))[0]
    shutil.copytree(os.path.join(resource, version),
                    os.path.join(out, "lib", "clang", version), dirs_exist_ok=True)
    # The compiler runtime and the unwinder come from the NDK: this clang is
    # the compiler only, and the pair it would look for in its own resource
    # directory are Android's.
    ndk_clang = os.path.join(ndk(), "toolchains", "llvm", "prebuilt", "linux-x86_64", "lib", "clang")
    ndk_version = sorted(os.listdir(ndk_clang))[0]
    runtimes = os.path.join(ndk_clang, ndk_version, "lib", "linux")
    for triple, builtins in (("aarch64-unknown-linux-android%d" % API,
                              "libclang_rt.builtins-aarch64-android.a"),):
        into = os.path.join(out, "lib", "clang", version, "lib", triple)
        os.makedirs(into, exist_ok=True)
        shutil.copy2(os.path.join(runtimes, builtins),
                     os.path.join(into, "libclang_rt.builtins.a"))
        shutil.copy2(os.path.join(runtimes, "aarch64", "libunwind.a"),
                     os.path.join(into, "libunwind.a"))
    print(f"{out}: clang {LLVM_VERSION} with the AArch64 __ptr32 patch")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--abi", choices=ABIS, default=ABIS[0],
                        help="which ABI's libraries to build (default %(default)s)")
    parser.add_argument("--with-llvm", action="store_true",
                        help="also build the patched clang the arm64 game needs")
    options = parser.parse_args()
    abi = options.abi
    os.chdir(ROOT)
    if options.with_llvm:
        llvm()
    out = os.path.join(OUT, abi)
    os.makedirs(out, exist_ok=True)
    sysroot = os.path.join(ndk(), "toolchains", "llvm", "prebuilt", "linux-x86_64", "sysroot")
    # Android's own zlib, whose directory is named after the ABI's triple.
    triple = "arm-linux-androideabi" if abi == "armeabi-v7a" else "aarch64-linux-android"
    zlib = [f"-DZLIB_INCLUDE_DIR={sysroot}/usr/include",
            f"-DZLIB_LIBRARY={sysroot}/usr/lib/{triple}/{API}/libz.so"]
    if not os.path.exists(os.path.join(out, "lib", "libpng16.a")):
        cmake_build(abi, "libpng", fetch("libpng"), out, "-DPNG_SHARED=OFF", "-DPNG_STATIC=ON",
                    "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF", *zlib)
    if not os.path.exists(os.path.join(out, "lib", "libfreetype.a")):
        cmake_build(abi, "freetype", fetch("freetype"), out, "-DBUILD_SHARED_LIBS=OFF",
                    "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON", "-DFT_DISABLE_BZIP2=ON",
                    "-DFT_REQUIRE_ZLIB=ON", "-DFT_REQUIRE_PNG=ON",
                    f"-DPNG_PNG_INCLUDE_DIR={out}/include", f"-DPNG_LIBRARY={out}/lib/libpng16.a",
                    *zlib)
    # SDL3: the library, its headers, and the Java glue the APK needs. It is
    # shared rather than static because its Java side calls into it by name.
    sdl_source = fetch("sdl")
    sdl_out = os.path.join(out, "sdl")
    if not os.path.exists(os.path.join(sdl_out, "lib", "libSDL3.so")):
        cmake_build(abi, "sdl", sdl_source, sdl_out, "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF")
    java = os.path.join(sdl_out, "java")
    if not os.path.exists(java):
        shutil.copytree(os.path.join(sdl_source, "android-project", "app", "src", "main", "java"), java)
    for needed in (f"{out}/lib/libfreetype.a", f"{out}/lib/libpng16.a",
                   f"{sdl_out}/lib/libSDL3.so", f"{sdl_out}/include/SDL3/SDL.h", java):
        if not os.path.exists(needed):
            sys.exit(f"{needed} was not produced")
    print(f"{out}: NDK {NDK_VERSION}, SDL3, FreeType and libpng for {abi}, API {API}")


if __name__ == "__main__":
    main()
