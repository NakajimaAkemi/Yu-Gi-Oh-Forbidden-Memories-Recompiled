/* The on-screen pad (touch_pad.h).
 *
 * Two clusters at the bottom corners, where thumbs rest: the directions on
 * the left, the four face buttons on the right, the shoulders above each and
 * Select and Start between them. The buttons carry the console's own symbols
 * rather than letters, so the shapes the game's prompts name are the shapes
 * on the glass.
 *
 * Diagonals come from the directions being a three-by-three grid: the four
 * edge cells are drawn and hold one bit each, and the four corner cells are
 * invisible and hold two, so a thumb between up and right presses both as a
 * real pad would.
 *
 * Everything is drawn into the overlay canvas the menu and the HUD share, in
 * software, because Android presents through SDL_Render (render/gl_none.c).
 * The shapes are distance fields: a pixel is covered by how far it is from
 * the outline, which costs a few multiplications per pixel and needs no art,
 * no font for the symbols and no extra texture. The canvas is only painted
 * again when something changes (TouchPad_Finger says so), so this runs on a
 * press, not per frame. */
#ifdef __ANDROID__
#include "touch_pad.h"
#include "controls.h"
#include "platform.h"
#include <math.h>
#include <string.h>

#define MAX_FINGERS 10
#define MAX_BUTTONS 20

enum Symbol { SYM_NONE, SYM_TRIANGLE, SYM_CIRCLE, SYM_CROSS, SYM_SQUARE, SYM_ARROW };

typedef struct {
    uint16_t bits;      /* what it presses; a corner cell holds two */
    int x, y, w, h;     /* the rectangle it occupies, in window pixels */
    int round;          /* a circle or a pill, rather than a square cell */
    int hidden;         /* a hit area with nothing drawn (the corners) */
    enum Symbol symbol;
    int turn;           /* an arrow's direction, in quarter turns from up */
    const char *label;  /* instead of a symbol */
} Button;

static Button buttons[MAX_BUTTONS];
static int button_count;
static int laid_w, laid_h;
static struct { int64_t finger; int button; } fingers[MAX_FINGERS];
static uint16_t held;

/* --- the canvas ------------------------------------------------------- */

/* Source-over, with `alpha` as coverage. The overlay starts transparent, so
 * the first shape on a pixel simply lands there. */
static void blend(MenuCanvas *canvas, int x, int y, uint32_t colour, float alpha)
{
    uint32_t *pixel, was_pixel, result;
    float was, now;
    int i;
    if (alpha <= 0.0f || x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) return;
    if (alpha > 1.0f) alpha = 1.0f;
    pixel = &canvas->pixels[(size_t)y * (size_t)canvas->stride + (size_t)x];
    was_pixel = *pixel;
    was = (float)((was_pixel >> 24) & 0xffu) / 255.0f;
    now = alpha + was * (1.0f - alpha);
    if (now <= 0.0f) {
        *pixel = 0;
        return;
    }
    /* Source-over with the colours kept unpremultiplied, which is what the
     * canvas holds: each channel is the weighted average of the two, divided
     * by the alpha it ends up with. */
    result = (uint32_t)(now * 255.0f + 0.5f) << 24;
    for (i = 16; i >= 0; i -= 8) {
        float source = (float)((colour >> i) & 0xffu) / 255.0f;
        float under = (float)((was_pixel >> i) & 0xffu) / 255.0f;
        float out = (source * alpha + under * was * (1.0f - alpha)) / now;
        if (out > 1.0f) out = 1.0f;
        result |= (uint32_t)(out * 255.0f + 0.5f) << i;
    }
    *pixel = result;
}

/* How much of a pixel a shape covers, from its signed distance: one pixel of
 * softness across the edge, which is enough to keep the circles smooth. */
static float coverage(float distance)
{
    float a = 0.5f - distance;
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

static float distance_to_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float length = dx * dx + dy * dy;
    float t = length > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / length : 0.0f;
    float cx, cy;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    cx = ax + t * dx;
    cy = ay + t * dy;
    return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

/* A closed outline of `count` points, `width` pixels thick. */
static void stroke(MenuCanvas *canvas, const float *points, int count, float width,
                   uint32_t colour, float alpha)
{
    float half = width * 0.5f;
    int x0 = canvas->width, y0 = canvas->height, x1 = 0, y1 = 0, i, x, y;
    for (i = 0; i < count; i++) {
        int px = (int)points[2 * i], py = (int)points[2 * i + 1];
        if (px - (int)half - 1 < x0) x0 = px - (int)half - 1;
        if (py - (int)half - 1 < y0) y0 = py - (int)half - 1;
        if (px + (int)half + 2 > x1) x1 = px + (int)half + 2;
        if (py + (int)half + 2 > y1) y1 = py + (int)half + 2;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float nearest = 1e9f;
            for (i = 0; i < count; i++) {
                int next = (i + 1) % count;
                float d = distance_to_segment((float)x + 0.5f, (float)y + 0.5f,
                                              points[2 * i], points[2 * i + 1],
                                              points[2 * next], points[2 * next + 1]);
                if (d < nearest) nearest = d;
            }
            blend(canvas, x, y, colour, coverage(nearest - half) * alpha);
        }
    }
}

static void fill_polygon(MenuCanvas *canvas, const float *points, int count, uint32_t colour, float alpha)
{
    /* Small convex shapes only (the arrows): a point is inside when it is on
     * the same side of every edge. */
    int x0 = canvas->width, y0 = canvas->height, x1 = 0, y1 = 0, i, x, y;
    for (i = 0; i < count; i++) {
        int px = (int)points[2 * i], py = (int)points[2 * i + 1];
        if (px - 1 < x0) x0 = px - 1;
        if (py - 1 < y0) y0 = py - 1;
        if (px + 2 > x1) x1 = px + 2;
        if (py + 2 > y1) y1 = py + 2;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float inside = 1e9f;
            for (i = 0; i < count; i++) {
                int next = (i + 1) % count;
                float ax = points[2 * i], ay = points[2 * i + 1];
                float bx = points[2 * next], by = points[2 * next + 1];
                float side = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                float length = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                float d = length > 0.0f ? side / length : 0.0f;
                if (d < inside) inside = d;
            }
            blend(canvas, x, y, colour, coverage(-inside) * alpha);
        }
    }
}

/* A rounded rectangle; a radius of half the shorter side makes a circle. */
static void fill_round_rect(MenuCanvas *canvas, int rx, int ry, int rw, int rh, float radius,
                            uint32_t colour, float alpha)
{
    float cx0 = (float)rx + radius, cx1 = (float)(rx + rw) - radius;
    float cy0 = (float)ry + radius, cy1 = (float)(ry + rh) - radius;
    int x, y;
    for (y = ry - 1; y < ry + rh + 1; y++) {
        for (x = rx - 1; x < rx + rw + 1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float qx = px < cx0 ? cx0 - px : px > cx1 ? px - cx1 : 0.0f;
            float qy = py < cy0 ? cy0 - py : py > cy1 ? py - cy1 : 0.0f;
            blend(canvas, x, y, colour, coverage(sqrtf(qx * qx + qy * qy) - radius) * alpha);
        }
    }
}

/* --- the layout ------------------------------------------------------- */

static void add(uint16_t bits, int x, int y, int w, int h, int round, enum Symbol symbol,
                int turn, const char *label, int hidden)
{
    Button *button;
    if (button_count >= MAX_BUTTONS) return;
    button = &buttons[button_count++];
    button->bits = bits;
    button->x = x;
    button->y = y;
    button->w = w;
    button->h = h;
    button->round = round;
    button->symbol = symbol;
    button->turn = turn;
    button->label = label;
    button->hidden = hidden;
}

void TouchPad_Layout(int window_w, int window_h)
{
    int unit = window_w < window_h ? window_w : window_h;
    int radius = unit * 11 / 100;              /* a face button, and a direction cell */
    int margin = unit * 6 / 100;
    int cell = radius * 9 / 10;
    int reach = radius * 23 / 10;              /* the diamond's half-width */
    int dpad_cx = margin + cell * 3 / 2, dpad_cy = window_h - margin - cell * 3 / 2;
    int face_cx = window_w - margin - reach, face_cy = window_h - margin - reach;
    int shoulder_w = radius * 2, shoulder_h = radius * 3 / 4;
    int shoulder_y = window_h - margin - cell * 3 - shoulder_h * 3 / 2;
    int pill_w = radius * 3 / 2, pill_h = radius * 2 / 3;
    int row, column;
    button_count = 0;
    laid_w = window_w;
    laid_h = window_h;
    /* The directions: the eight cells around the middle of a 3x3 grid. The
     * corners come first so that they win where they meet an edge cell. */
    for (row = -1; row <= 1; row++) {
        for (column = -1; column <= 1; column++) {
            uint16_t bits = 0;
            int corner = row != 0 && column != 0;
            if (!row && !column) continue;
            if (row < 0) bits |= CTRL_DEST_UP;
            if (row > 0) bits |= CTRL_DEST_DOWN;
            if (column < 0) bits |= CTRL_DEST_LEFT;
            if (column > 0) bits |= CTRL_DEST_RIGHT;
            if (!corner) continue;
            add(bits, dpad_cx + column * cell - cell / 2, dpad_cy + row * cell - cell / 2,
                cell, cell, 0, SYM_NONE, 0, NULL, 1);
        }
    }
    add(CTRL_DEST_UP, dpad_cx - cell / 2, dpad_cy - cell * 3 / 2, cell, cell, 0, SYM_ARROW, 0, NULL, 0);
    add(CTRL_DEST_RIGHT, dpad_cx + cell / 2, dpad_cy - cell / 2, cell, cell, 0, SYM_ARROW, 1, NULL, 0);
    add(CTRL_DEST_DOWN, dpad_cx - cell / 2, dpad_cy + cell / 2, cell, cell, 0, SYM_ARROW, 2, NULL, 0);
    add(CTRL_DEST_LEFT, dpad_cx - cell * 3 / 2, dpad_cy - cell / 2, cell, cell, 0, SYM_ARROW, 3, NULL, 0);
    /* The face buttons, in the console's own places. */
    add(CTRL_DEST_TRIANGLE, face_cx - radius, face_cy - reach - radius, radius * 2, radius * 2, 1,
        SYM_TRIANGLE, 0, NULL, 0);
    add(CTRL_DEST_CIRCLE, face_cx + reach - radius, face_cy - radius, radius * 2, radius * 2, 1,
        SYM_CIRCLE, 0, NULL, 0);
    add(CTRL_DEST_CROSS, face_cx - radius, face_cy + reach - radius, radius * 2, radius * 2, 1,
        SYM_CROSS, 0, NULL, 0);
    add(CTRL_DEST_SQUARE, face_cx - reach - radius, face_cy - radius, radius * 2, radius * 2, 1,
        SYM_SQUARE, 0, NULL, 0);
    /* The shoulders, above the cluster each belongs to. */
    add(CTRL_DEST_L2, margin, shoulder_y - shoulder_h * 5 / 4, shoulder_w, shoulder_h, 1, SYM_NONE, 0, "L2", 0);
    add(CTRL_DEST_L1, margin, shoulder_y, shoulder_w, shoulder_h, 1, SYM_NONE, 0, "L1", 0);
    add(CTRL_DEST_R2, window_w - margin - shoulder_w, shoulder_y - shoulder_h * 5 / 4,
        shoulder_w, shoulder_h, 1, SYM_NONE, 0, "R2", 0);
    add(CTRL_DEST_R1, window_w - margin - shoulder_w, shoulder_y, shoulder_w, shoulder_h, 1,
        SYM_NONE, 0, "R1", 0);
    /* Select and Start, between the thumbs. */
    add(CTRL_DEST_SELECT, window_w / 2 - pill_w - pill_w / 4, window_h - margin - pill_h,
        pill_w, pill_h, 1, SYM_NONE, 0, "SEL", 0);
    add(CTRL_DEST_START, window_w / 2 + pill_w / 4, window_h - margin - pill_h,
        pill_w, pill_h, 1, SYM_NONE, 0, "START", 0);
}

int TouchPad_Wanted(void)
{
    /* A real pad takes over: its player does not want half the screen under
     * buttons they are not touching. */
    return !Gamepad_Connected(0);
}

/* --- touches ---------------------------------------------------------- */

static int button_at(int x, int y)
{
    int i;
    for (i = 0; i < button_count; i++) {
        const Button *button = &buttons[i];
        if (button->round) {
            float rx = (float)button->w * 0.5f, ry = (float)button->h * 0.5f;
            float dx = ((float)x - ((float)button->x + rx)) / rx;
            float dy = ((float)y - ((float)button->y + ry)) / ry;
            /* A little past the drawn edge, so a thumb's centre need not be
             * exactly on it. */
            if (dx * dx + dy * dy <= 1.44f) return i;
        } else if (x >= button->x && y >= button->y &&
                   x < button->x + button->w && y < button->y + button->h) {
            return i;
        }
    }
    return -1;
}

static uint16_t recompute(void)
{
    uint16_t bits = 0;
    int i;
    for (i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].button >= 0 && fingers[i].button < button_count) {
            bits |= buttons[fingers[i].button].bits;
        }
    }
    return bits;
}

int TouchPad_Finger(int64_t finger, int down, int x, int y)
{
    int slot = -1, free_slot = -1, i;
    uint16_t before = held;
    if (!button_count) return 0;
    for (i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].button >= 0 && fingers[i].finger == finger) slot = i;
        if (fingers[i].button < 0 && free_slot < 0) free_slot = i;
    }
    if (!down) {
        if (slot >= 0) fingers[slot].button = -1;
    } else {
        int at = button_at(x, y);
        if (slot < 0) slot = free_slot;
        if (slot < 0) return 0; /* more fingers than a pad has buttons */
        fingers[slot].finger = finger;
        /* Sliding off a button releases it, as sliding onto one presses it:
         * the directions are one grid, so a thumb can roll around it. */
        fingers[slot].button = at;
    }
    held = recompute();
    return held != before;
}

void TouchPad_Release(void)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++) {
        fingers[i].button = -1;
    }
    held = 0;
}

uint16_t TouchPad_Bits(void)
{
    return TouchPad_Wanted() ? held : 0;
}

/* --- drawing ---------------------------------------------------------- */

#define PAD_FILL 0x101828u
#define PAD_RIM 0x6d7b96u
#define PAD_SYMBOL 0xd6deeeu
#define PAD_LIT_FILL 0x3a4c74u
#define PAD_LIT_RIM 0xe8c66au

static void draw_symbol(MenuCanvas *canvas, const Button *button, uint32_t colour, float alpha)
{
    float cx = (float)button->x + (float)button->w * 0.5f;
    float cy = (float)button->y + (float)button->h * 0.5f;
    float r = (float)(button->w < button->h ? button->w : button->h) * 0.26f;
    float thickness = r * 0.34f;
    float points[8];
    if (thickness < 2.0f) thickness = 2.0f;
    switch (button->symbol) {
    case SYM_TRIANGLE:
        points[0] = cx; points[1] = cy - r;
        points[2] = cx + r * 0.92f; points[3] = cy + r * 0.72f;
        points[4] = cx - r * 0.92f; points[5] = cy + r * 0.72f;
        stroke(canvas, points, 3, thickness, colour, alpha);
        break;
    case SYM_SQUARE:
        points[0] = cx - r * 0.8f; points[1] = cy - r * 0.8f;
        points[2] = cx + r * 0.8f; points[3] = cy - r * 0.8f;
        points[4] = cx + r * 0.8f; points[5] = cy + r * 0.8f;
        points[6] = cx - r * 0.8f; points[7] = cy + r * 0.8f;
        stroke(canvas, points, 4, thickness, colour, alpha);
        break;
    case SYM_CROSS: {
        float arm = r * 0.78f;
        float a[4], b[4];
        a[0] = cx - arm; a[1] = cy - arm; a[2] = cx + arm; a[3] = cy + arm;
        b[0] = cx + arm; b[1] = cy - arm; b[2] = cx - arm; b[3] = cy + arm;
        stroke(canvas, a, 2, thickness, colour, alpha);
        stroke(canvas, b, 2, thickness, colour, alpha);
        break;
    }
    case SYM_CIRCLE: {
        int x, y;
        float outer = r * 0.86f;
        for (y = (int)(cy - outer) - 2; y < (int)(cy + outer) + 2; y++) {
            for (x = (int)(cx - outer) - 2; x < (int)(cx + outer) + 2; x++) {
                float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                float d = sqrtf(dx * dx + dy * dy) - outer;
                blend(canvas, x, y, colour, coverage(fabsf(d) - thickness * 0.5f) * alpha);
            }
        }
        break;
    }
    case SYM_ARROW: {
        /* An upward triangle, turned into place. */
        float base[6];
        int i;
        base[0] = 0.0f; base[1] = -r * 0.85f;
        base[2] = r * 0.8f; base[3] = r * 0.5f;
        base[4] = -r * 0.8f; base[5] = r * 0.5f;
        for (i = 0; i < 3; i++) {
            float px = base[2 * i], py = base[2 * i + 1], tx = px, ty = py;
            int turn;
            for (turn = 0; turn < button->turn; turn++) {
                float nx = -ty, ny = tx;   /* a quarter turn clockwise on screen */
                tx = nx;
                ty = ny;
            }
            points[2 * i] = cx + tx;
            points[2 * i + 1] = cy + ty;
        }
        fill_polygon(canvas, points, 3, colour, alpha);
        break;
    }
    case SYM_NONE:
    default:
        break;
    }
}

void TouchPad_Draw(MenuCanvas *canvas)
{
    int i;
    if (!TouchPad_Wanted()) return;
    if (canvas->width != laid_w || canvas->height != laid_h) {
        TouchPad_Layout(canvas->width, canvas->height);
    }
    for (i = 0; i < button_count; i++) {
        const Button *button = &buttons[i];
        int lit = (held & button->bits) == button->bits && button->bits != 0;
        float radius = button->round ? (float)(button->w < button->h ? button->w : button->h) * 0.5f
                                     : (float)button->w * 0.22f;
        uint32_t fill = lit ? PAD_LIT_FILL : PAD_FILL;
        uint32_t rim = lit ? PAD_LIT_RIM : PAD_RIM;
        if (button->hidden) continue;
        fill_round_rect(canvas, button->x, button->y, button->w, button->h, radius, fill,
                        lit ? 0.72f : 0.42f);
        /* The rim: the same shape one pixel thinner, drawn as an outline. */
        {
            float points[8];
            float inset = 1.5f;
            points[0] = (float)button->x + inset; points[1] = (float)button->y + inset;
            points[2] = (float)(button->x + button->w) - inset; points[3] = (float)button->y + inset;
            points[4] = (float)(button->x + button->w) - inset;
            points[5] = (float)(button->y + button->h) - inset;
            points[6] = (float)button->x + inset; points[7] = (float)(button->y + button->h) - inset;
            if (!button->round) stroke(canvas, points, 4, 2.0f, rim, lit ? 0.95f : 0.55f);
        }
        if (button->round) {
            int x, y;
            float cx = (float)button->x + (float)button->w * 0.5f;
            float cy = (float)button->y + (float)button->h * 0.5f;
            float rx = (float)button->w * 0.5f - 1.0f, ry = (float)button->h * 0.5f - 1.0f;
            for (y = button->y - 1; y < button->y + button->h + 1; y++) {
                for (x = button->x - 1; x < button->x + button->w + 1; x++) {
                    float dx = ((float)x + 0.5f - cx) / rx, dy = ((float)y + 0.5f - cy) / ry;
                    float scale = rx < ry ? rx : ry;
                    float d = (sqrtf(dx * dx + dy * dy) - 1.0f) * scale;
                    blend(canvas, x, y, rim, coverage(fabsf(d) - 1.0f) * (lit ? 0.95f : 0.55f));
                }
            }
        }
        if (button->label) {
            /* Menu_DrawTextScaled sizes the face at 13 pixels per unit of
             * scale and puts the text's middle on `y`, so the label is sized
             * to about half the pill's height and then shrunk until it fits
             * across. The colour is plain RGB, as the menu's own calls pass. */
            int scale = button->h / 26;
            int width;
            if (scale < 1) scale = 1;
            while (scale > 1 && Menu_TextWidthScaled(button->label, scale) > button->w - 4) {
                scale--;
            }
            width = Menu_TextWidthScaled(button->label, scale);
            Menu_DrawTextScaled(canvas, button->x + (button->w - width) / 2,
                                button->y + button->h / 2, button->label,
                                lit ? PAD_LIT_RIM : PAD_SYMBOL, scale);
        } else {
            draw_symbol(canvas, button, lit ? PAD_LIT_RIM : PAD_SYMBOL, lit ? 1.0f : 0.78f);
        }
    }
}

void TouchPad_Bounds(int *x, int *y, int *w, int *h)
{
    int x0 = laid_w, y0 = laid_h, x1 = 0, y1 = 0, i;
    *x = *y = *w = *h = 0;
    if (!TouchPad_Wanted() || !button_count) return;
    for (i = 0; i < button_count; i++) {
        const Button *button = &buttons[i];
        if (button->hidden) continue;
        if (button->x - 2 < x0) x0 = button->x - 2;
        if (button->y - 2 < y0) y0 = button->y - 2;
        if (button->x + button->w + 2 > x1) x1 = button->x + button->w + 2;
        if (button->y + button->h + 2 > y1) y1 = button->y + button->h + 2;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > laid_w) x1 = laid_w;
    if (y1 > laid_h) y1 = laid_h;
    if (x1 <= x0 || y1 <= y0) return;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
}
#else
/* No touchscreen to draw a pad on: the port's other systems have a keyboard
 * and take controllers, so these report a pad that is not wanted and never
 * pressed. sdl.c calls them unconditionally rather than carry an #ifdef
 * around each one. */
#include "touch_pad.h"

int TouchPad_Wanted(void) { return 0; }

void TouchPad_Layout(int window_w, int window_h)
{
    (void)window_w;
    (void)window_h;
}

int TouchPad_Finger(int64_t finger, int down, int x, int y)
{
    (void)finger; (void)down; (void)x; (void)y;
    return 0;
}

void TouchPad_Release(void) {}

uint16_t TouchPad_Bits(void) { return 0; }

void TouchPad_Draw(MenuCanvas *canvas) { (void)canvas; }

void TouchPad_Bounds(int *x, int *y, int *w, int *h)
{
    *x = *y = *w = *h = 0;
}
#endif
