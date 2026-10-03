/* Custom artwork for the cards mods add (cards.h, notes/more-cards.md).
 *
 * A card's art record on the disc (7 sectors in WA_MRG.MRG, read by
 * func_80029164) is, in its first 0x3060 bytes:
 *
 *   +0x0000  102 x 96 art, one byte per pixel      (indices 1-255)
 *   +0x2640  its CLUT, 256 BGR555 entries          (entry 0 unused, 0x8000)
 *   +0x2840  the title plate, 96 x 14 at 4 bits    (0 clear, 1 darkest ink to 7 faintest)
 *   +0x2AE0  40 x 32 thumbnail, one byte per pixel (indices 1-63)
 *   +0x2FE0  its CLUT, 64 BGR555 entries
 *
 * and the thumbnail block (+0x2AE0, 0x580 bytes) is also the card's own sector
 * the duel reads for the hand and field. This file makes those bytes from a
 * PNG: cropped to the shape, averaged down to the size, and reduced to the
 * colours by median cut. The plate is the card's name set in Times (the
 * system's; the retail plates' own face is not available as a font), or a
 * PNG the mod gives.
 *
 * The plate is drawn subtractively over the card's gold frame through a
 * fixed 16-entry CLUT (index 1 a light grey, 7 a dark one), so index 1 takes
 * the most away: it is the darkest ink, and 7 barely shows. The retail
 * plates are authored that way, stems at 1 with faint 6 and 7 fringes. The
 * measurements, and the settings below that make a legible plate (Times
 * regular at 13 pixels, baseline under row 11, whole-pixel advances), are the
 * YuGiOhForbiddenMemoriesRecomp project's (src/psx_card_packs.c,
 * render_title), found against window captures; each texel takes the ink of
 * the nearest tone (ink_of). Besides a mod's own cards, a retail card a
 * translation renames gets its plate from here (cards.c, translated_plate). */
#include "pc/compat/fs.h"
#include "cards.h"
#include "art.h"
#include "pc/text/glyphs.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#elif defined(__ANDROID__)
#include "pc/platform/android_fonts.h"
#else
#include <fontconfig/fontconfig.h>
#endif

typedef struct { unsigned char r, g, b; } Rgb;

/* --- images ---------------------------------------------------------- */

/* The PNG as RGB over black or, with `ink`, as ink coverage in all three
 * channels: dark and opaque is full ink, so a strip drawn in black on white
 * and one drawn on a transparent background read the same. */
static Rgb *load_png_as(const char *path, int *width, int *height, int ink)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *out;
    size_t i, count;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return NULL; }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        return NULL;
    }
    fclose(file);
    count = (size_t)image.width * image.height;
    out = malloc(count * sizeof(*out));
    for (i = 0; out && i < count; i++) {
        unsigned a = rgba[i * 4 + 3];
        if (ink) {
            unsigned luma = (rgba[i * 4] * 3u + rgba[i * 4 + 1] * 6u + rgba[i * 4 + 2]) / 10u;
            out[i].r = out[i].g = out[i].b = (unsigned char)((255 - luma) * a / 255);
            continue;
        }
        out[i].r = (unsigned char)(rgba[i * 4] * a / 255);
        out[i].g = (unsigned char)(rgba[i * 4 + 1] * a / 255);
        out[i].b = (unsigned char)(rgba[i * 4 + 2] * a / 255);
    }
    *width = (int)image.width;
    *height = (int)image.height;
    free(rgba);
    png_image_free(&image);
    return out;
}

static Rgb *load_png(const char *path, int *width, int *height)
{
    return load_png_as(path, width, height, 0);
}

/* `w` x `h` from the middle of the image at that shape, each pixel the
 * average of the source pixels under it. */
static void resample(const Rgb *source, int sw, int sh, Rgb *out, int w, int h)
{
    double cw = sw, ch = sh, x0, y0;
    int x, y;
    if (cw * h > ch * w) cw = ch * w / h; else ch = cw * h / w;
    x0 = (sw - cw) / 2;
    y0 = (sh - ch) / 2;
    for (y = 0; y < h; y++) {
        int top = (int)(y0 + ch * y / h), bottom = (int)(y0 + ch * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)(x0 + cw * x / w), right = (int)(x0 + cw * (x + 1) / w), sx, sy;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    const Rgb *p = &source[(size_t)sy * sw + sx];
                    r += p->r; g += p->g; b += p->b; n++;
                }
            }
            if (!n) n = 1;
            out[y * w + x].r = (unsigned char)(r / n);
            out[y * w + x].g = (unsigned char)(g / n);
            out[y * w + x].b = (unsigned char)(b / n);
        }
    }
}

/* --- colours --------------------------------------------------------- */

typedef struct { int first, count; } Box;

static unsigned short to555(int r, int g, int b)
{
    unsigned short c = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    return c ? c : 0x8000;   /* 0 is the transparent colour; this is black */
}

static int channel(const Rgb *c, int axis) { return axis == 0 ? c->r : axis == 1 ? c->g : c->b; }
static int sort_axis;
/* By one channel, then the other two: a whole order, so every C library's
 * qsort sorts alike and a picture is made the same on Linux and Windows. */
static int by_axis(const void *a, const void *b)
{
    int k, d = channel(a, sort_axis) - channel(b, sort_axis);
    for (k = 0; !d && k < 3; k++) d = channel(a, k) - channel(b, k);
    return d;
}

/* Median cut of the pixels to `colours` entries, written to `clut` from
 * entry 1 on, and each pixel's entry to `indices`. */
static void quantize(const Rgb *pixels, int count, int colours, unsigned short *clut, unsigned char *indices)
{
    Rgb *sorted = malloc((size_t)count * sizeof(*sorted)), palette[256];
    Box boxes[256];
    int box_count = 1, i, k;
    if (!sorted) return;
    memcpy(sorted, pixels, (size_t)count * sizeof(*sorted));
    boxes[0].first = 0;
    boxes[0].count = count;
    while (box_count < colours) {
        int best = -1, best_range = 0, axis = 0;
        for (i = 0; i < box_count; i++) {
            int a, low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
            if (boxes[i].count < 2) continue;
            for (k = boxes[i].first; k < boxes[i].first + boxes[i].count; k++) {
                for (a = 0; a < 3; a++) {
                    int v = channel(&sorted[k], a);
                    if (v < low[a]) low[a] = v;
                    if (v > high[a]) high[a] = v;
                }
            }
            for (a = 0; a < 3; a++) {
                if (high[a] - low[a] > best_range) { best_range = high[a] - low[a]; best = i; axis = a; }
            }
        }
        if (best < 0 || best_range < 4) break;   /* no box worth splitting */
        sort_axis = axis;
        qsort(sorted + boxes[best].first, (size_t)boxes[best].count, sizeof(*sorted), by_axis);
        boxes[box_count].first = boxes[best].first + boxes[best].count / 2;
        boxes[box_count].count = boxes[best].count - boxes[best].count / 2;
        boxes[best].count /= 2;
        box_count++;
    }
    for (i = 0; i < box_count; i++) {
        unsigned long r = 0, g = 0, b = 0, n = (unsigned long)boxes[i].count;
        for (k = boxes[i].first; k < boxes[i].first + boxes[i].count; k++) {
            r += sorted[k].r; g += sorted[k].g; b += sorted[k].b;
        }
        if (!n) n = 1;
        palette[i].r = (unsigned char)(r / n);
        palette[i].g = (unsigned char)(g / n);
        palette[i].b = (unsigned char)(b / n);
        clut[i + 1] = to555(palette[i].r, palette[i].g, palette[i].b);
    }
    for (; i < colours; i++) clut[i + 1] = 0x8000;
    clut[0] = 0x8000;
    for (k = 0; k < count; k++) {
        int best = 0;
        long best_distance = -1;
        for (i = 0; i < box_count; i++) {
            long dr = pixels[k].r - palette[i].r, dg = pixels[k].g - palette[i].g, db = pixels[k].b - palette[i].b;
            long distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (best_distance < 0 || distance < best_distance) { best_distance = distance; best = i; }
        }
        indices[k] = (unsigned char)(best + 1);
    }
    free(sorted);
}

static void put_clut(unsigned char *out, const unsigned short *clut, int entries)
{
    int i;
    for (i = 0; i < entries; i++) {
        out[i * 2] = (unsigned char)(clut[i] & 0xFF);
        out[i * 2 + 1] = (unsigned char)(clut[i] >> 8);
    }
}

static int image_into(const char *path, unsigned char *record, int thumbnail_only, char *why, size_t why_size)
{
    int width, height;
    Rgb *source = load_png(path, &width, &height), *art;
    unsigned short clut[256];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT * sizeof(*art));
    if (!art) { free(source); return 0; }
    if (!thumbnail_only) {
        resample(source, width, height, art, CARD_ART_WIDTH, CARD_ART_HEIGHT);
        quantize(art, CARD_ART_WIDTH * CARD_ART_HEIGHT, 255, clut, record + CARD_ART_PIXELS);
        put_clut(record + CARD_ART_CLUT, clut, 256);
    }
    resample(source, width, height, art, CARD_THUMB_WIDTH, CARD_THUMB_HEIGHT);
    quantize(art, CARD_THUMB_WIDTH * CARD_THUMB_HEIGHT, 63, clut, record + CARD_THUMB_PIXELS);
    put_clut(record + CARD_THUMB_CLUT, clut, 64);
    free(art);
    free(source);
    return 1;
}

int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 0, why, why_size);
}

int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 1, why, why_size);
}

/* Like load_png, but keeps the PNG's own alpha instead of flattening it
 * away: field_art's cutouts (below) need to know what to leave out; every
 * other user of a card's art draws a full rectangle, so nothing else does.
 * *alpha is the same length as the returned pixels, NULL on failure along
 * with the return. */
static Rgb *load_png_rgba(const char *path, int *width, int *height, unsigned char **alpha)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *out;
    size_t i, count;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return NULL; }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        return NULL;
    }
    fclose(file);
    count = (size_t)image.width * image.height;
    out = malloc(count * sizeof(*out));
    *alpha = malloc(count);
    if (!out || !*alpha) {
        free(out);
        free(*alpha);
        *alpha = NULL;
        free(rgba);
        png_image_free(&image);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        out[i].r = rgba[i * 4];
        out[i].g = rgba[i * 4 + 1];
        out[i].b = rgba[i * 4 + 2];
        (*alpha)[i] = rgba[i * 4 + 3];
    }
    *width = (int)image.width;
    *height = (int)image.height;
    free(rgba);
    png_image_free(&image);
    return out;
}

/* resample's own crop-and-average, for a single channel: called with the
 * same source and target sizes resample itself is, so the same crop window
 * (a deterministic function of those sizes alone) lines back up with it,
 * texel for texel. */
static void resample_alpha(const unsigned char *source, int sw, int sh, unsigned char *out, int w, int h)
{
    double cw = sw, ch = sh, x0, y0;
    int x, y;
    if (cw * h > ch * w) cw = ch * w / h; else ch = cw * h / w;
    x0 = (sw - cw) / 2;
    y0 = (sh - ch) / 2;
    for (y = 0; y < h; y++) {
        int top = (int)(y0 + ch * y / h), bottom = (int)(y0 + ch * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)(x0 + cw * x / w), right = (int)(x0 + cw * (x + 1) / w), sx, sy;
            unsigned long a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    a += source[(size_t)sy * sw + sx];
                    n++;
                }
            }
            if (!n) n = 1;
            out[y * w + x] = (unsigned char)(a / n);
        }
    }
}

#define FIELD_ART_ALPHA_CUTOFF 128

int CardArt_FieldArtFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    int width, height, i;
    unsigned char *alpha;
    Rgb *source = load_png_rgba(path, &width, &height, &alpha), *art;
    unsigned char *art_alpha;
    unsigned short clut[256];

    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT * sizeof(*art));
    art_alpha = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT);
    if (!art || !art_alpha) {
        free(source);
        free(alpha);
        free(art);
        free(art_alpha);
        return 0;
    }
    resample(source, width, height, art, CARD_ART_WIDTH, CARD_ART_HEIGHT);
    resample_alpha(alpha, width, height, art_alpha, CARD_ART_WIDTH, CARD_ART_HEIGHT);
    quantize(art, CARD_ART_WIDTH * CARD_ART_HEIGHT, 255, clut, record + CARD_ART_PIXELS);
    /* A texel this transparent is written as index 0 (never quantize's own
     * output: every real pixel is 1-255), and its CLUT entry set to the
     * PS1's own transparent colour, 0x0000 (to555's own comment) -- unlike
     * quantize's own default for it, 0x8000, opaque black, since index 0
     * never reaches a real pixel anywhere else a card's art is drawn. */
    clut[0] = 0;
    for (i = 0; i < CARD_ART_WIDTH * CARD_ART_HEIGHT; i++) {
        if (art_alpha[i] < FIELD_ART_ALPHA_CUTOFF) record[CARD_ART_PIXELS + i] = 0;
    }
    put_clut(record + CARD_ART_CLUT, clut, 256);

    free(source);
    free(alpha);
    free(art);
    free(art_alpha);
    return 1;
}

/* A Free Duel portrait record: the 48x48 image at 8 bits a pixel, then its
 * 64-entry palette, which is what the screen uploads and the disc holds forty
 * of (notes/more-duelists.md). The same shape as a card's, at another size. */
int CardArt_PortraitFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    int width, height;
    Rgb *source = load_png(path, &width, &height), *art;
    unsigned short clut[256];

    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc((size_t)PORTRAIT_SIDE * PORTRAIT_SIDE * sizeof(*art));
    if (!art) { free(source); return 0; }
    resample(source, width, height, art, PORTRAIT_SIDE, PORTRAIT_SIDE);
    quantize(art, PORTRAIT_SIDE * PORTRAIT_SIDE, 63, clut, record);
    put_clut(record + PORTRAIT_PIXELS, clut, 64);
    free(art);
    free(source);
    return 1;
}

/* A guardian star's icon (stars.h): 16x16 at 4 bits a pixel, index 0 where
 * the PNG is less than half covered. With `palette` (16 colours, 0 the
 * transparent one) each pixel takes the nearest of its entries 1-15, as the
 * disc's stars are drawn; without, the PNG's own 15 colours go to `clut`.
 * The image is taken whole, squeezed to a square if it is not one. */
int CardArt_IconFromImage(const char *path, const unsigned short *palette, unsigned char *pixels,
                          unsigned short *clut, char *why, size_t why_size)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb cells[CARD_ICON_SIDE * CARD_ICON_SIDE], opaque[CARD_ICON_SIDE * CARD_ICON_SIDE];
    unsigned char covered[CARD_ICON_SIDE * CARD_ICON_SIDE], indices[CARD_ICON_SIDE * CARD_ICON_SIDE];
    int x, y, count = 0, i;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!(file = fopen(path, "rb")) || !png_image_begin_read_from_stdio(&image, file)) {
        if (file) fclose(file);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    fclose(file);
    /* Each cell the average of the pixels under it, weighted by their
     * alpha, and how much of it they cover. */
    for (y = 0; y < CARD_ICON_SIDE; y++) {
        int top = (int)((long)image.height * y / CARD_ICON_SIDE);
        int bottom = (int)((long)image.height * (y + 1) / CARD_ICON_SIDE);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < CARD_ICON_SIDE; x++) {
            int left = (int)((long)image.width * x / CARD_ICON_SIDE);
            int right = (int)((long)image.width * (x + 1) / CARD_ICON_SIDE), sx, sy;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < (int)image.height; sy++) {
                for (sx = left; sx < right && sx < (int)image.width; sx++) {
                    const unsigned char *p = &rgba[((size_t)sy * image.width + sx) * 4];
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            }
            i = y * CARD_ICON_SIDE + x;
            covered[i] = n && a * 2 >= n * 255;
            if (a) {
                cells[i].r = (unsigned char)(r / a);
                cells[i].g = (unsigned char)(g / a);
                cells[i].b = (unsigned char)(b / a);
            } else {
                cells[i].r = cells[i].g = cells[i].b = 0;
            }
            if (covered[i]) opaque[count++] = cells[i];
        }
    }
    free(rgba);
    png_image_free(&image);
    memset(indices, 0, sizeof(indices));
    if (palette) {
        for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE; i++) {
            long best_distance = -1;
            int entry;
            if (!covered[i]) continue;
            for (entry = 1; entry < 16; entry++) {
                unsigned short c = palette[entry];
                long dr, dg, db, distance;
                if (!(c & 0x7FFF) && !(c & 0x8000)) continue;   /* transparent */
                dr = cells[i].r - (long)((c & 0x1F) << 3);
                dg = cells[i].g - (long)(((c >> 5) & 0x1F) << 3);
                db = cells[i].b - (long)(((c >> 10) & 0x1F) << 3);
                distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (best_distance < 0 || distance < best_distance) { best_distance = distance; indices[i] = (unsigned char)entry; }
            }
        }
        if (clut) memcpy(clut, palette, 16 * sizeof(*clut));
    } else {
        unsigned short own[256];
        unsigned char chosen[CARD_ICON_SIDE * CARD_ICON_SIDE];
        int k = 0;
        memset(own, 0, sizeof(own));
        if (count) quantize(opaque, count, 15, own, chosen);
        for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE; i++) {
            if (covered[i]) indices[i] = chosen[k++];
        }
        own[0] = 0;   /* index 0 stays transparent */
        if (clut) memcpy(clut, own, 16 * sizeof(*clut));
    }
    for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE / 2; i++) {
        pixels[i] = (unsigned char)(indices[i * 2] | (indices[i * 2 + 1] << 4));
    }
    return 1;
}

/* Where resample takes a `w` by `h` picture from: the middle of the image
 * at that shape, in whole pixels, and the image's size. Only the PNG's
 * header is read. */
int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw, int *ch, int *width, int *height)
{
    png_image image;
    FILE *file;
    double sw, sh, fw, fh;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return 0; }
    sw = fw = image.width;
    sh = fh = image.height;
    png_image_free(&image);
    fclose(file);
    if (fw * h > fh * w) fw = fh * w / h; else fh = fw * h / w;
    *cw = (int)(fw + 0.5);
    *ch = (int)(fh + 0.5);
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
    *x = (int)((sw - *cw) / 2 + 0.5);
    *y = (int)((sh - *ch) / 2 + 0.5);
    *width = (int)sw;
    *height = (int)sh;
    return 1;
}

/* A PNG with see-through parts as 8-bit texels (title_images.c): stretched
 * to `w` x `h`, each texel the average of the pixels under it, a texel under
 * half covered clear (entry 0, the PS1's transparent 0x0000) and the rest
 * reduced to 255 colours by median cut from entry 1. */
int CardArt_IndexedImage(const char *path, int w, int h, unsigned char *indices, unsigned short *clut, char *why,
                         size_t why_size)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *opaque;
    unsigned char *opaque_indices;
    int x, y, sw, sh, count = 0, k;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file || !png_image_begin_read_from_stdio(&image, file)) {
        if (file) fclose(file);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    fclose(file);
    sw = (int)image.width;
    sh = (int)image.height;
    png_image_free(&image);
    opaque = malloc((size_t)w * h * sizeof(*opaque));
    opaque_indices = malloc((size_t)w * h);
    if (!opaque || !opaque_indices) {
        free(rgba);
        free(opaque);
        free(opaque_indices);
        snprintf(why, why_size, "out of memory for %s", path);
        return 0;
    }
    for (y = 0; y < h; y++) {
        int top = (int)((long)sh * y / h), bottom = (int)((long)sh * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)((long)sw * x / w), right = (int)((long)sw * (x + 1) / w), sx, sy;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    const unsigned char *p = rgba + ((size_t)sy * sw + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            }
            if (!n || a * 2 < n * 255) {
                indices[y * w + x] = 0;
                continue;
            }
            opaque[count].r = (unsigned char)(r / a);
            opaque[count].g = (unsigned char)(g / a);
            opaque[count].b = (unsigned char)(b / a);
            indices[y * w + x] = 1;   /* an opaque texel, numbered below */
            count++;
        }
    }
    free(rgba);
    memset(clut, 0, 256 * sizeof(*clut));
    if (count) quantize(opaque, count, 255, clut, opaque_indices);
    clut[0] = 0x0000;
    for (k = 0, x = 0; x < w * h; x++) {
        if (indices[x]) indices[x] = opaque_indices[k++];
    }
    free(opaque);
    free(opaque_indices);
    return 1;
}

/* The PNG's width and height, from its header. */
int CardArt_ImageSize(const char *path, int *width, int *height)
{
    int x, y, cw, ch;
    return CardArt_Crop(path, 1, 1, &x, &y, &cw, &ch, width, height);
}

/* --- the title plate --------------------------------------------------- */

static void put_ink(unsigned char *plate, int x, int y, int ink)
{
    unsigned char *byte;
    if (x < 0 || x >= CARD_TITLE_WIDTH || y < 0 || y >= CARD_TITLE_HEIGHT) return;
    byte = plate + y * (CARD_TITLE_WIDTH / 2) + x / 2;
    if (x & 1) *byte = (unsigned char)((*byte & 0x0F) | (ink << 4));
    else *byte = (unsigned char)((*byte & 0xF0) | ink);
}

/* Coverage (0-255) to the plate ink of the nearest tone. What each ink
 * takes from the gold under it, as a share of what 1 takes, measured on a
 * retail plate drawn in the game (Dancing Elf's, at 1x): 1 all of it, then
 * about .93, .8, .6, .5, .35 and .18 for 7; 0 none. The thresholds are the
 * midpoints between those, so an edge is drawn in the ink that darkens it as
 * much as its coverage says, stems at 1 and fringes at 6 and 7 as the retail
 * plates have them. */
static int ink_of(int coverage)
{
    static const unsigned char from[7] = {247, 221, 179, 140, 109, 69, 23};
    int ink;
    for (ink = 0; ink < 7; ink++) {
        if (coverage >= from[ink]) return ink + 1;
    }
    return 0;
}

int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why, size_t why_size)
{
    int width, height, x, y;
    Rgb *source = load_png_as(path, &width, &height, 1), small[CARD_TITLE_WIDTH * CARD_TITLE_HEIGHT];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    resample(source, width, height, small, CARD_TITLE_WIDTH, CARD_TITLE_HEIGHT);
    memset(plate, 0, CARD_TITLE_BYTES);
    for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
        for (x = 0; x < CARD_TITLE_WIDTH; x++) {
            put_ink(plate, x, y, ink_of(small[y * CARD_TITLE_WIDTH + x].r));
        }
    }
    free(source);
    return 1;
}

static FT_Library library;
static FT_Face face;
static int face_tried;

static const char *serif_file(void)
{
#ifdef _WIN32
    return Win32_SerifFontPath();
#elif defined(__ANDROID__)
    return Android_FontPath(ANDROID_FONT_SERIF);
#else
    static char path[1024];
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
    if (!FcInit()) return NULL;
    pattern = FcNameParse((const FcChar8 *)"Times:regular");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    path[0] = '\0';
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        snprintf(path, sizeof(path), "%s", (const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return path[0] ? path : NULL;
#endif
}

void *CardArt_SerifFace(void)
{
    if (!face_tried) {
        const char *file = serif_file();
        face_tried = 1;
        if (!file || FT_Init_FreeType(&library) || FT_New_Face(library, file, 0, &face)) face = NULL;
    }
    return face;
}

/* The name as the retail plates set theirs: Times at 13 pixels, the
 * baseline under row 11, from column 3, each glyph on a whole pixel so stems
 * fill whole columns; a name wider than 90 pixels is squeezed into columns
 * 3 to 93, as the long retail names are. At `factor` pixels per texel all of
 * that is `factor` times as big. `cover` gets each pixel's coverage, 0-255,
 * CARD_TITLE_WIDTH * factor across and CARD_TITLE_HEIGHT * factor down.
 * 0 when there is no serif font. */
static int set_name(const char *name, int factor, unsigned char *cover)
{
    const int wide = 512 * factor, baseline = 11 * factor, left = 3 * factor, room = 90 * factor;
    const int width = CARD_TITLE_WIDTH * factor, height = CARD_TITLE_HEIGHT * factor, above = 8 * factor;
    unsigned char *line, *drawn;
    int pen = left, x, y, ink_low = wide, ink_high = -1, previous = 0, first = above;
    const char *c = name;
    CardArt_SerifFace();
    if (!face || FT_Set_Pixel_Sizes(face, 0, 13 * factor)) return 0;
    /* Drawn with `above` rows over the plate, for marks that reach past
     * its top (a Vietnamese capital's two). */
    drawn = calloc((size_t)(above + height) * wide, 1);
    if (!drawn) return 0;
    line = drawn + (size_t)above * wide;
    while (*c) {
        /* A character at a time, as the name's glyphs are (cards.c,
         * encode_name): an accented letter is one glyph, not two. */
        FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)Glyphs_NextCharacter(&c));
        FT_Bitmap *bitmap;
        if (!index) continue; /* a letter the face lacks is left out, not drawn as a box */
        if (previous && index && FT_HAS_KERNING(face)) {
            FT_Vector kern;
            if (!FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kern)) pen += (int)((kern.x + 32) >> 6);
        }
        previous = index;
        if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) continue;
        bitmap = &face->glyph->bitmap;
        for (y = 0; y < (int)bitmap->rows; y++) {
            int ty = baseline - face->glyph->bitmap_top + y;
            if (ty < -above || ty >= height) continue;
            for (x = 0; x < (int)bitmap->width; x++) {
                int tx = pen + face->glyph->bitmap_left + x;
                unsigned char v = bitmap->buffer[y * bitmap->pitch + x];
                if (tx < 0 || tx >= wide || !v) continue;
                if (v > line[ty * wide + tx]) line[ty * wide + tx] = v;
                if (ty < first) first = ty;
                if (tx < ink_low) ink_low = tx;
                if (tx > ink_high) ink_high = tx;
            }
        }
        pen += (int)((face->glyph->advance.x + 32) >> 6);
        if (pen >= wide) break;
    }
    if (first < 0) {
        /* Ink above the plate: what is above the baseline is fitted into
         * the rows above it (each row the most of the ones it takes), so
         * the marks stay on; the baseline and what is below stay. A name
         * that fits (any ASCII one) is left as it is. */
        unsigned char *fitted = calloc((size_t)baseline * wide, 1);
        if (!fitted) {
            free(drawn);
            return 0;
        }
        for (y = 0; y < baseline; y++) {
            int from = first + y * (baseline - first) / baseline, to = first + (y + 1) * (baseline - first) / baseline, k;
            unsigned char *row = fitted + (size_t)y * wide;
            if (to <= from) to = from + 1;
            for (k = from; k < to; k++) {
                for (x = 0; x < wide; x++) {
                    if (line[(ptrdiff_t)k * wide + x] > row[x]) row[x] = line[(ptrdiff_t)k * wide + x];
                }
            }
        }
        memcpy(line, fitted, (size_t)baseline * wide);
        free(fitted);
    }
    memset(cover, 0, (size_t)width * height);
    if (ink_high < ink_low) {
    } else if (ink_high - ink_low + 1 <= room) {
        for (y = 0; y < height; y++) memcpy(cover + (size_t)y * width, line + (size_t)y * wide, (size_t)width);
    } else {
        /* Squeezed with a linear filter, then brought back up to full ink:
         * averaging thins every stem, and thin stems are faint ones. */
        float *squeezed = malloc(sizeof(float) * (size_t)height * room);
        float step = (float)(ink_high - ink_low + 1) / room, peak = 1.0f;
        if (!squeezed) {
            free(drawn);
            return 0;
        }
        for (y = 0; y < height; y++) {
            for (x = 0; x < room; x++) {
                float at = ink_low + (x + 0.5f) * step - 0.5f, t;
                int i0, i1;
                if (at < 0) at = 0;
                i0 = (int)at;
                t = at - i0;
                i1 = i0 + 1 < wide ? i0 + 1 : i0;
                squeezed[y * room + x] = line[y * wide + i0] * (1.0f - t) + line[y * wide + i1] * t;
                if (squeezed[y * room + x] > peak) peak = squeezed[y * room + x];
            }
        }
        for (y = 0; y < height; y++) {
            for (x = 0; x < room; x++) {
                cover[(size_t)y * width + left + x] = (unsigned char)(int)(squeezed[y * room + x] * 255.0f / peak + 0.5f);
            }
        }
        free(squeezed);
    }
    free(drawn);
    return 1;
}

int CardArt_TitleFromName(const char *name, unsigned char *plate)
{
    unsigned char cover[CARD_TITLE_WIDTH * CARD_TITLE_HEIGHT];
    int x, y;
    if (!set_name(name, 1, cover)) return 0;
    memset(plate, 0, CARD_TITLE_BYTES);
    for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
        for (x = 0; x < CARD_TITLE_WIDTH; x++) put_ink(plate, x, y, ink_of(cover[y * CARD_TITLE_WIDTH + x]));
    }
    return 1;
}

int CardArt_TitlePicture(const char *name, int factor, unsigned char *indices, int pitch)
{
    const int width = CARD_TITLE_WIDTH * factor, height = CARD_TITLE_HEIGHT * factor;
    unsigned char *cover = malloc((size_t)width * height);
    int x, y;
    if (!cover) return 0;
    if (!set_name(name, factor, cover)) {
        free(cover);
        return 0;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            /* The plate's inks run from 1 (full, the darkest) to 7 (the
             * faintest the retail plates use); at this size the whole run. */
            int c = cover[(size_t)y * width + x], ink = 0;
            if (c >= 24) ink = 7 - (int)((c >= 200 ? 176 : c - 24) * 6 / 176.0 + 0.5);
            indices[(size_t)y * pitch + x] = (unsigned char)ink;
        }
    }
    free(cover);
    return 1;
}
