/* Where Android keeps its fonts (android_fonts.h). The names, newest first:
 * Noto is the system family on every Android that runs this, shipped as
 * variable fonts on recent releases and static ones before that, and Droid
 * was its predecessor through Android 4. Each list ends with a face from the
 * other family, so that a device missing one still gets text rather than
 * none. */
#ifdef __ANDROID__
/* access() through the UTF-8 boundary, as every unit that names a file does
 * (pc/compat/fs.h, notes/pc-build.md "Folder names"); the header comes first,
 * as in the other units, and unistd.h follows it for R_OK. */
#include "pc/compat/fs.h"
#include "android_fonts.h"
#include <stddef.h>
#include <unistd.h>

const char *Android_FontPath(int kind)
{
    static const char *const serif_names[] = {
        "/system/fonts/NotoSerif-Regular.ttf",
        "/system/fonts/NotoSerif[wdth,wght].ttf",
        "/system/fonts/DroidSerif-Regular.ttf",
        "/system/fonts/Roboto-Regular.ttf",
    };
    static const char *const sans_names[] = {
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/Roboto[wdth,wght].ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
    };
    /* The CJK face is a collection (.ttc) holding all four languages; the
     * first face in it is the one FreeType opens at index 0, and the glyphs
     * the kanji ROM wants are in every one of them. */
    static const char *const cjk_names[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansJP-Regular.otf",
        "/system/fonts/NotoSansCJKjp-Regular.otf",
        "/system/fonts/DroidSansJapanese.ttf",
    };
    static const char *const bold_names[] = {
        "/system/fonts/Roboto-Bold.ttf",
        "/system/fonts/Roboto[wdth,wght].ttf", /* variable: bold is an instance of it */
        "/system/fonts/NotoSans-Bold.ttf",
        "/system/fonts/DroidSans-Bold.ttf",
    };
    const char *const *names = kind == ANDROID_FONT_BOLD ? bold_names :
                               kind == ANDROID_FONT_CJK ? cjk_names :
                               kind == ANDROID_FONT_SERIF ? serif_names : sans_names;
    size_t count = kind == ANDROID_FONT_BOLD ? sizeof(bold_names) / sizeof(*bold_names) :
                   kind == ANDROID_FONT_CJK ? sizeof(cjk_names) / sizeof(*cjk_names) :
                   kind == ANDROID_FONT_SERIF ? sizeof(serif_names) / sizeof(*serif_names)
                                              : sizeof(sans_names) / sizeof(*sans_names);
    size_t i;
    for (i = 0; i < count; i++) {
        if (access(names[i], R_OK) == 0) {
            return names[i];
        }
    }
    return NULL;
}
#else
/* Not this system's: the translation unit stays, empty but for this. */
typedef int memories_android_fonts_unused;
#endif
