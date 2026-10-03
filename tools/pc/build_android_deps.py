#!/usr/bin/env python3
"""Fetch and build what the Android game library needs.

The Linux build takes its libraries from Debian 11 and the Windows one from
llvm-mingw (tools/pc/build_linux_sysroot.py, tools/pc/build_win32_deps.py).
This is the same thing for Android, under tmp/pc/android-deps:

  * the NDK, which is the compiler (ndk/);
  * the SDK's platform, build tools and cmake, which tools/pc/build_apk.py
    packages with (sdk/);
  * SDL3 built for armeabi-v7a, and its Java glue, which the APK carries
    (sdl/);
  * static libpng and FreeType, linked into the library (include/, lib/).

The libpng and FreeType archives are the ones the Windows build pins, so the
three platforms link the same versions. zlib is not among them: Android ships
one, as it ships GLES and the log library.

ANDROID_NDK and ANDROID_HOME are used when they already name an installation,
so a machine with the SDK set up downloads neither. Nothing needs installing
by hand.

API and ABI must stay what tools/pc/build_game32.py compiles for.
"""
import hashlib, os, shutil, stat, subprocess, sys, tarfile, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tmp", "pc", "android-deps")
DOWNLOADS = os.path.join(OUT, "downloads")
ABI = "armeabi-v7a"
API = 28
NDK_VERSION = "r27c"
BUILD_TOOLS = "34.0.0"
PLATFORM = "android-34"
CMAKE_VERSION = "3.22.1"
ARCHIVES = {
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
    if not os.path.exists(archive) or hashlib.sha256(open(archive, "rb").read()).hexdigest() != digest:
        print(f"fetching {url}")
        urllib.request.urlretrieve(url, archive)
        got = hashlib.sha256(open(archive, "rb").read()).hexdigest()
        if got != digest:
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


def cmake_build(name, source, out, *options):
    """Configure and install one library for the ABI with the NDK."""
    toolchain = os.path.join(ndk(), "build", "cmake", "android.toolchain.cmake")
    cmake = os.path.join(sdk(), "cmake", CMAKE_VERSION, "bin", "cmake")
    build = os.path.join(OUT, "build", name)
    run([cmake, "-S", source, "-B", build, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={ABI}",
         f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={out}", *options],
        env={**os.environ, "PATH": os.path.dirname(cmake) + os.pathsep + os.environ.get("PATH", "")})
    run([cmake, "--build", build, "--target", "install"])


def main():
    os.chdir(ROOT)
    os.makedirs(OUT, exist_ok=True)
    sysroot = os.path.join(ndk(), "toolchains", "llvm", "prebuilt", "linux-x86_64", "sysroot")
    zlib = [f"-DZLIB_INCLUDE_DIR={sysroot}/usr/include",
            f"-DZLIB_LIBRARY={sysroot}/usr/lib/arm-linux-androideabi/{API}/libz.so"]
    if not os.path.exists(os.path.join(OUT, "lib", "libpng16.a")):
        cmake_build("libpng", fetch("libpng"), OUT, "-DPNG_SHARED=OFF", "-DPNG_STATIC=ON",
                    "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF", *zlib)
    if not os.path.exists(os.path.join(OUT, "lib", "libfreetype.a")):
        cmake_build("freetype", fetch("freetype"), OUT, "-DBUILD_SHARED_LIBS=OFF",
                    "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON", "-DFT_DISABLE_BZIP2=ON",
                    "-DFT_REQUIRE_ZLIB=ON", "-DFT_REQUIRE_PNG=ON",
                    f"-DPNG_PNG_INCLUDE_DIR={OUT}/include", f"-DPNG_LIBRARY={OUT}/lib/libpng16.a", *zlib)
    # SDL3: the library, its headers, and the Java glue the APK needs. It is
    # shared rather than static because its Java side calls into it by name.
    sdl_source = fetch("sdl")
    sdl_out = os.path.join(OUT, "sdl")
    if not os.path.exists(os.path.join(sdl_out, "lib", "libSDL3.so")):
        cmake_build("sdl", sdl_source, sdl_out, "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF")
    java = os.path.join(sdl_out, "java")
    if not os.path.exists(java):
        shutil.copytree(os.path.join(sdl_source, "android-project", "app", "src", "main", "java"), java)
    for needed in (f"{OUT}/lib/libfreetype.a", f"{OUT}/lib/libpng16.a",
                   f"{sdl_out}/lib/libSDL3.so", f"{sdl_out}/include/SDL3/SDL.h", java):
        if not os.path.exists(needed):
            sys.exit(f"{needed} was not produced")
    print(f"{OUT}: NDK {NDK_VERSION}, SDL3, FreeType and libpng for {ABI}, API {API}")


if __name__ == "__main__":
    main()
