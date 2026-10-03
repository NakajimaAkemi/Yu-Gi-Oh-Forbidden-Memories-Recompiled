#ifndef MEMORIES_PC_PLATFORM_ANDROID_FONTS_H
#define MEMORIES_PC_PLATFORM_ANDROID_FONTS_H
/* Android has no fontconfig: its system faces are files under /system/fonts
 * with known names. The counterpart of Win32_FontPath and
 * Win32_SerifFontPath (win32.h), for the menu's text and the card plates. */

#define ANDROID_FONT_SANS 0  /* the menu's text */
#define ANDROID_FONT_SERIF 1 /* what the retail card plates set names in */
#define ANDROID_FONT_CJK 2   /* the kanji ROM's glyphs */
#define ANDROID_FONT_BOLD 3  /* the HD text's bold sans */

/* A readable system face of that kind, or NULL. The returned string is
 * static. */
const char *Android_FontPath(int kind);

#endif
