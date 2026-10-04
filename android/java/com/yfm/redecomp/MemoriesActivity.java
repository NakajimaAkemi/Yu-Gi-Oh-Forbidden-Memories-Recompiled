package com.yfm.redecomp;

import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * The game as an Android activity (notes/android-build.md).
 *
 * SDL owns the window, the input and the audio here as it does on the
 * desktop, so this adds only what an app has to do for itself:
 *
 *  - it names the library the game is in, which SDL then looks SDL_main up
 *    in (libmemories.so, built by tools/pc/build_game32.py --target android);
 *  - it unpacks what a desktop release would have shipped beside the
 *    executable -- the mods and the languages -- out of the APK's assets,
 *    and the disc image too when one was built in;
 *  - it tells the game where that folder is (nativeSetPaths), because an app
 *    has no command line and no program directory to find.
 *
 * One folder holds everything: the app's own directory on shared storage,
 * which any file manager can reach without a storage permission. The player
 * puts their disc image in the "game" folder inside it, and their saves,
 * save states, screenshots and settings appear beside it.
 */
public class MemoriesActivity extends SDLActivity {
    private static final String TAG = "memories";
    /** Assets unpacked from the APK; everything else in it is left alone.
     * The mod SDK a desktop release carries is not among them: a mod is
     * built on a computer, not here. The disc image, when the APK carries
     * one, is unpacked separately: it is hundreds of megabytes, and doing
     * that on the thread that draws would have Android stop the app for not
     * responding. */
    private static final String[] UNPACKED = { "mods", "languages" };
    /** Where a built-in disc image lives in the assets, if there is one. */
    private static final String DISC_ASSETS = "game";

    /** Tells the game the two roots paths.c would otherwise work out itself. */
    private static native void nativeSetPaths(String programDir, String userDir);

    @Override
    protected String[] getLibraries() {
        /* SDL takes the last of these as the one holding SDL_main. */
        return new String[] { "SDL3", "memories" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        /* Before SDL loads anything: the game reads this folder as it starts. */
        try {
            unpackAssets();
        } catch (IOException failure) {
            /* The game still runs; a missing mod or language is not fatal,
             * and its own message says what it could not find. */
            Log.e(TAG, "cannot unpack the game's files", failure);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void main() {
        /* Called on SDL's own thread once the libraries are loaded, which is
         * what nativeSetPaths needs, and before SDL_main, which is what the
         * paths are for. The disc is unpacked here rather than in onCreate
         * because it takes the better part of a minute: this thread is not
         * the one Android watches, so a long wait here delays the game
         * rather than ending it. */
        File root = filesRoot();
        try {
            unpackDisc(root);
        } catch (IOException failure) {
            /* The game says what it could not find, and the player can still
             * put an image in the folder themselves. */
            Log.e(TAG, "cannot unpack the disc image", failure);
        }
        nativeSetPaths(root.getAbsolutePath(), root.getAbsolutePath());
        super.main();
    }

    /**
     * Copy a disc image built into the APK into the game folder, once. An
     * APK without one does nothing here, and so does one whose image is
     * already unpacked: the file is checked by name and size, since what is
     * in the assets cannot change without the version changing too.
     */
    private void unpackDisc(File root) throws IOException {
        String[] names = getAssets().list(DISC_ASSETS);
        if (names == null || names.length == 0) {
            return; /* no disc built in: the player brings their own */
        }
        File game = new File(root, "game");
        if (!game.isDirectory() && !game.mkdirs()) {
            throw new IOException("cannot create " + game);
        }
        /* A stamp rather than a comparison: measuring a compressed asset
         * means inflating all of it, which is the very cost being avoided.
         * The version covers it, since the assets cannot change without it
         * changing too. */
        File stamp = new File(game, ".disc-unpacked");
        String want = version();
        if (want.equals(read(stamp))) {
            return;
        }
        for (String name : names) {
            Log.i(TAG, "unpacking " + name + "; this happens once and takes a while");
            copyFile(getAssets(), DISC_ASSETS + "/" + name, new File(game, name));
            Log.i(TAG, "unpacked " + name);
        }
        write(stamp, want);
    }

    /**
     * The one folder: /sdcard/Android/data/com.yfm.redecomp/files, or the
     * app's private directory when shared storage is not there (a device
     * with no emulated storage, or storage that is busy being ejected).
     */
    private File filesRoot() {
        File shared = getExternalFilesDir(null);
        return shared != null ? shared : getFilesDir();
    }

    /**
     * Copy the asset trees out of the APK, once per installed version. The
     * stamp holds the version and the time it was installed, so a reinstall
     * or an update unpacks again and an ordinary start does not.
     */
    private void unpackAssets() throws IOException {
        File root = filesRoot();
        File stamp = new File(root, ".unpacked");
        String want = version();
        if (want.equals(read(stamp))) {
            ensureGameFolder(root);
            return;
        }
        AssetManager assets = getAssets();
        for (String tree : UNPACKED) {
            copyTree(assets, tree, new File(root, tree));
        }
        ensureGameFolder(root);
        write(stamp, want);
        Log.i(TAG, "unpacked the game's files into " + root);
    }

    /** Where the player puts the disc image, with a note saying so. */
    private void ensureGameFolder(File root) throws IOException {
        File game = new File(root, "game");
        if (!game.isDirectory() && !game.mkdirs()) {
            throw new IOException("cannot create " + game);
        }
        File note = new File(game, "PUT-YOUR-DISC-IMAGE-HERE.txt");
        String[] built_in = getAssets().list(DISC_ASSETS);
        if (built_in != null && built_in.length > 0) {
            return; /* the APK carries one; the player need not find theirs */
        }
        if (!note.exists()) {
            write(note, "Put your own copy of the Yu-Gi-Oh! Forbidden Memories disc in this\n"
                      + "folder: the raw .bin file of a .bin/.cue pair, from the USA release\n"
                      + "(SLUS-01411). Any name ending in .bin will do.\n\n"
                      + "No game data is included with this app, and none is downloaded.\n");
        }
    }

    private String version() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName + " " + info.lastUpdateTime;
        } catch (Exception failure) {
            return "unknown";
        }
    }

    /** Recursively copy one asset directory. Files are replaced wholesale. */
    private void copyTree(AssetManager assets, String asset, File destination) throws IOException {
        String[] names = assets.list(asset);
        if (names == null || names.length == 0) {
            copyFile(assets, asset, destination);
            return;
        }
        if (!destination.isDirectory() && !destination.mkdirs()) {
            throw new IOException("cannot create " + destination);
        }
        for (String name : names) {
            copyTree(assets, asset + "/" + name, new File(destination, name));
        }
    }

    private void copyFile(AssetManager assets, String asset, File destination) throws IOException {
        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        byte[] buffer = new byte[65536];
        InputStream in = assets.open(asset);
        try {
            OutputStream out = new FileOutputStream(destination);
            try {
                for (int got = in.read(buffer); got > 0; got = in.read(buffer)) {
                    out.write(buffer, 0, got);
                }
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
    }

    private static String read(File file) {
        try {
            byte[] bytes = new byte[(int) Math.min(file.length(), 4096L)];
            InputStream in = new java.io.FileInputStream(file);
            try {
                int got = in.read(bytes);
                return got > 0 ? new String(bytes, 0, got, "UTF-8") : "";
            } finally {
                in.close();
            }
        } catch (IOException missing) {
            return "";
        }
    }

    private static void write(File file, String text) throws IOException {
        OutputStream out = new FileOutputStream(file);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }
}
