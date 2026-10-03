/* The OpenGL presenter, absent. Android's GL is GLES, and both gl_picture.c
 * and present_pass.c are written against the desktop fixed-function pipeline
 * -- glBegin, glMatrixMode, glOrtho, glPushMatrix -- which GLES has never
 * had. The Android build therefore takes the SDL_Render path that sdl.c
 * already keeps for machines where no GL context can be made: every call
 * into the OpenGL presenter there is behind `use_gl`, which stays 0.
 *
 * These definitions stand in for the two files so the library links. Each
 * reports the pass as off, which is what sdl.c does with them when the GL
 * context is missing, and none of them is reached while use_gl is 0.
 *
 * Replacing this with a real GLES presenter is a self-contained piece of
 * work: the picture is one textured quad and the effects are fragment
 * shaders, so it is the fixed-function scaffolding around them, not the
 * drawing, that has to be rewritten. */
#include "gl_picture.h"
#include "present_pass.h"

int GlPicture_Init(void) { return 0; }
int GlPicture_Replay(void) { return 0; }
int GlPicture_Scale(void) { return 1; }
int GlPicture_Behind(void) { return 0; }

unsigned GlPicture_Texture(int *picture_w, int *picture_h)
{
    if (picture_w) *picture_w = 0;
    if (picture_h) *picture_h = 0;
    return 0;
}

unsigned GlPicture_ShownTexture(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}

int GlPicture_Read(int x, int y, int w, int h, uint32_t *out)
{
    (void)x; (void)y; (void)w; (void)h; (void)out;
    return 0;
}

unsigned GlPicture_WideTexture(int x, int y, int w, int h, int *picture_w, int *picture_h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (picture_w) *picture_w = 0;
    if (picture_h) *picture_h = 0;
    return 0;
}

int GlPicture_ReadWide(int x, int y, int w, int h, int wide_w, int want_scale, uint32_t *out)
{
    (void)x; (void)y; (void)w; (void)h; (void)wide_w; (void)want_scale; (void)out;
    return 0;
}

int PresentPass_Wanted(void) { return 0; }

int PresentPass_Begin(unsigned texture, int source_h, float s0, float t0, float s1, float t1,
                      int textures_smoothed)
{
    (void)texture; (void)source_h; (void)s0; (void)t0; (void)s1; (void)t1; (void)textures_smoothed;
    return 0;
}

void PresentPass_End(void) {}

/* The fixed-function entry points sdl.c's own presenter calls. GLES exports
 * the rest of what it uses -- textures, state, glReadPixels, glGetString --
 * but never had the matrix stack or immediate mode, so these eight would be
 * the only unresolved symbols in the library. They are defined here rather
 * than guarded at each of their call sites, which would thread an #ifdef
 * through the whole presenter for code that cannot run: every one of them
 * sits behind `use_gl`, and on Android create_window never makes a context,
 * so use_gl stays 0 and none of these is ever entered. If one is, the abort
 * says so rather than drawing nothing.
 *
 * SDL_opengl.h declares the full desktop API on every platform, so the
 * prototypes these match are the ones the presenter compiled against. */
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>

static void unreachable_gl(const char *name)
{
    fprintf(stderr, "memories-pc: %s reached with no OpenGL presenter\n", name);
    abort();
}

void glBegin(GLenum mode) { (void)mode; unreachable_gl("glBegin"); }
void glEnd(void) { unreachable_gl("glEnd"); }
void glVertex2f(GLfloat x, GLfloat y) { (void)x; (void)y; unreachable_gl("glVertex2f"); }
void glTexCoord2f(GLfloat s, GLfloat t) { (void)s; (void)t; unreachable_gl("glTexCoord2f"); }
void glMatrixMode(GLenum mode) { (void)mode; unreachable_gl("glMatrixMode"); }
void glLoadIdentity(void) { unreachable_gl("glLoadIdentity"); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    (void)r; (void)g; (void)b; (void)a;
    unreachable_gl("glColor4f");
}
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val)
{
    (void)left; (void)right; (void)bottom; (void)top; (void)near_val; (void)far_val;
    unreachable_gl("glOrtho");
}
