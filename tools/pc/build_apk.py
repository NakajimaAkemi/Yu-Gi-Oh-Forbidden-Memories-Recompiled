#!/usr/bin/env python3
"""Package the Android library built by build_game32.py as an installable APK.

Run tools/pc/build_game32.py --target android first; this takes what it left
in tmp/pc/android and wraps it:

  * the resources and the manifest go through aapt2 (android/);
  * SDL3's Java glue and the activity (android/java) are compiled against
    android.jar and dexed with d8;
  * --disc builds a disc image into the APK, under assets/game/, which the
    activity unpacks into the player's game folder the first time it runs.
    No image is included otherwise, and none is in the repository: it is the
    player's own copy, and an APK with one in it is theirs alone and not to
    be passed on;
  * every ABI that has been built goes in lib/<abi>/, each with its own
    libmemories.so and libSDL3.so, so one APK installs on a 32-bit device
    and on a 64-bit-only one alike; the mods and the languages -- what a
    desktop release ships beside the executable -- go in assets/, which the
    activity unpacks on first start;
  * zipalign and apksigner finish it.

No Gradle: the SDK's own tools do each step, which keeps the Android build the
same kind of thing as the other two (one Python driver, no project files to
keep in step). The APK is signed with a debug key, generated on first use --
enough to install by hand, not to publish.

tools/pc/build_android_deps.py fetches the NDK and the SDK and builds the
libraries; ANDROID_HOME and ANDROID_NDK are honoured when already set.
"""
import argparse, glob, os, shutil, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEPS = os.environ.get("MEMORIES_ANDROID_DEPS") or "tmp/pc/android-deps"
SDK = os.environ.get("ANDROID_HOME") or f"{DEPS}/sdk"
BUILD_TOOLS_VERSION = os.environ.get("MEMORIES_ANDROID_BUILD_TOOLS") or "34.0.0"
PLATFORM = os.environ.get("MEMORIES_ANDROID_PLATFORM") or "android-34"
MIN_SDK, TARGET_SDK = 28, 34
# Where tools/pc/build_game32.py leaves each ABI's library. One that has not
# been built is left out rather than being an error: a 32-bit-only APK is
# still useful, and the arm64 one needs the patched clang.
ABI_BUILDS = {"armeabi-v7a": "tmp/pc/android", "arm64-v8a": "tmp/pc/android-arm64"}


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if result.returncode:
        sys.exit(f"{command[0]} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def tool(name):
    path = f"{SDK}/build-tools/{BUILD_TOOLS_VERSION}/{name}"
    if not os.path.exists(path):
        sys.exit(f"{path} is missing: run tools/pc/build_android_deps.py")
    return path


def java_sources(*roots):
    found = []
    for root in roots:
        found += sorted(glob.glob(f"{root}/**/*.java", recursive=True))
    if not found:
        sys.exit(f"no Java sources under {', '.join(roots)}")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--abi", action="append", choices=list(ABI_BUILDS),
                        help="only this ABI (repeatable; default every one that is built)")
    parser.add_argument("--out", default="tmp/pc/android/memories.apk", help="the APK to write")
    parser.add_argument("--disc", help="a disc image (.bin, and its .cue beside it) to build in")
    # Not under the dependencies: re-fetching those would make a new key, and
    # an APK signed with a different one than the copy already on the device
    # cannot replace it -- Android refuses it as "App not installed" until the
    # old one is uninstalled. Keeping it beside them survives that.
    parser.add_argument("--keystore", default="tmp/pc/android-debug.keystore")
    options = parser.parse_args()
    os.chdir(ROOT)
    wanted = options.abi or list(ABI_BUILDS)
    abis = {abi: ABI_BUILDS[abi] for abi in wanted
            if os.path.exists(f"{ABI_BUILDS[abi]}/libmemories.so")}
    if not abis:
        sys.exit("no library to package: run tools/pc/build_game32.py --target android "
                 "(and --target android-arm64 for the 64-bit one) first")
    # The assets are the same whichever ABI built them.
    build = next(iter(abis.values()))
    out = options.out
    work = "tmp/pc/apk"
    android_jar = f"{SDK}/platforms/{PLATFORM}/android.jar"
    for abi in abis:
        sdl = f"{DEPS}/{abi}/sdl/lib/libSDL3.so"
        if not os.path.exists(sdl):
            sys.exit(f"{sdl} is missing: run tools/pc/build_android_deps.py --abi {abi}")
    # SDL's Java glue is the same for every ABI; take the first one's.
    sdl_java = f"{DEPS}/{next(iter(abis))}/sdl/java"
    for needed in (android_jar, sdl_java):
        if not os.path.exists(needed):
            sys.exit(f"{needed} is missing: run tools/pc/build_android_deps.py")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(f"{work}/classes", exist_ok=True)

    # The release's data, as the activity expects to find it in the APK.
    assets = f"{work}/assets"
    for tree in ("mods", "languages"):
        if os.path.isdir(f"{build}/{tree}"):
            shutil.copytree(f"{build}/{tree}", f"{assets}/{tree}")
    os.makedirs(assets, exist_ok=True)
    if options.disc:
        if not os.path.exists(options.disc):
            sys.exit(f"{options.disc}: no such disc image")
        os.makedirs(f"{assets}/game", exist_ok=True)
        for source in [options.disc] + ([os.path.splitext(options.disc)[0] + ".cue"]
                                        if os.path.exists(os.path.splitext(options.disc)[0] + ".cue")
                                        else []):
            into = f"{assets}/game/{os.path.basename(source)}"
            try:
                os.link(source, into)  # the image is large; do not copy it twice
            except OSError:
                shutil.copy2(source, into)
        print(f"{options.disc}: built in ({os.path.getsize(options.disc) >> 20} MiB)")

    # Resources and manifest.
    run([tool("aapt2"), "compile", "--dir", "android/res", "-o", f"{work}/res.zip"])
    run([tool("aapt2"), "link", "-o", f"{work}/base.apk", "--manifest", "android/AndroidManifest.xml",
         "-I", android_jar, "-A", assets,
         "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK),
         f"{work}/res.zip"])

    # SDL's Java glue and the activity.
    sources = java_sources(sdl_java, "android/java")
    run(["javac", "-nowarn", "-source", "8", "-target", "8", "-bootclasspath", android_jar,
         "-classpath", android_jar, "-d", f"{work}/classes", *sources])
    classes = sorted(glob.glob(f"{work}/classes/**/*.class", recursive=True))
    os.makedirs(f"{work}/dex", exist_ok=True)  # d8 writes into a directory that is already there
    run([tool("d8"), "--release", "--min-api", str(MIN_SDK), "--lib", android_jar,
         "--output", f"{work}/dex", *classes])

    # The code and the libraries into the resource APK.
    shutil.copy(f"{work}/base.apk", f"{work}/unaligned.apk")
    with zipfile.ZipFile(f"{work}/unaligned.apk", "a", zipfile.ZIP_DEFLATED) as apk:
        for dex in sorted(glob.glob(f"{work}/dex/*.dex")):
            apk.write(dex, os.path.basename(dex))
        for abi, abi_build in abis.items():
            apk.write(f"{abi_build}/libmemories.so", f"lib/{abi}/libmemories.so")
            apk.write(f"{DEPS}/{abi}/sdl/lib/libSDL3.so", f"lib/{abi}/libSDL3.so")

    run([tool("zipalign"), "-f", "4", f"{work}/unaligned.apk", f"{work}/aligned.apk"])
    if not os.path.exists(options.keystore):
        os.makedirs(os.path.dirname(options.keystore) or ".", exist_ok=True)
        run(["keytool", "-genkeypair", "-keystore", options.keystore, "-alias", "androiddebugkey",
             "-storepass", "android", "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10950", "-dname", "CN=Forbidden Memories Recompiled debug"])
        print(f"{options.keystore}: generated a debug signing key")
    # apksigner's defaults write the v1, v2 and v3 signatures, so the APK
    # installs by hand as well as through adb. `apksigner verify` then reports
    # only v3 as used, which is the scheme a minSdk 28 platform goes by, not a
    # missing signature: `verify --min-sdk-version 21` shows all three.
    run([tool("apksigner"), "sign", "--ks", options.keystore, "--ks-pass", "pass:android",
         "--ks-key-alias", "androiddebugkey", "--key-pass", "pass:android",
         "--out", out, f"{work}/aligned.apk"])
    run([tool("apksigner"), "verify", out])
    # Android 11 and later refuse an APK whose resources.arsc is compressed or
    # is not 4-byte aligned, and signing happens after zipalign, so the result
    # is what gets checked.
    run([tool("zipalign"), "-c", "4", out])
    size = os.path.getsize(out)
    print(f"{out}: {size // 1024} KiB, {', '.join(abis)}, minSdk {MIN_SDK}")
    print("install it with: adb install -r " + out)


if __name__ == "__main__":
    main()
