/* What the activity tells the game before SDL_main runs.
 *
 * An Android app has no command line and no process environment of its own
 * to inherit, so the two roots paths.c would work out for itself
 * (Paths_ProgramDir, Paths_UserDir) are set from Java instead. Both point at
 * the folder the activity unpacked the release into, which on Android is the
 * app's own directory on shared storage: the player can reach it with any
 * file manager to drop their disc image in and to get at their saves and
 * screenshots, and no storage permission is needed for it.
 *
 * This is the only JNI entry the library has. The version script exports
 * Java_* along with SDL_main (tools/pc/build_game32.py). */
#ifdef __ANDROID__
#include <jni.h>
/* setenv() through the UTF-8 boundary (pc/compat/fs.h): the folder these name
 * is the player's own and may be spelled with anything. */
#include "pc/compat/fs.h"
#include <stdlib.h>

JNIEXPORT void JNICALL
Java_com_yfm_redecomp_MemoriesActivity_nativeSetPaths(JNIEnv *env, jclass class,
                                                      jstring program_dir, jstring user_dir)
{
    const char *text;
    (void)class;
    if (program_dir && (text = (*env)->GetStringUTFChars(env, program_dir, NULL)) != NULL) {
        setenv("MEMORIES_PROGRAM_DIR", text, 1);
        (*env)->ReleaseStringUTFChars(env, program_dir, text);
    }
    if (user_dir && (text = (*env)->GetStringUTFChars(env, user_dir, NULL)) != NULL) {
        setenv("MEMORIES_USER_DIR", text, 1);
        (*env)->ReleaseStringUTFChars(env, user_dir, text);
    }
}
#else
/* Not this system's: the translation unit stays, empty but for this. */
typedef int memories_android_jni_unused;
#endif
