#include "graphics.h"
#if SOFTTV
#include "backplane_data_tv.h"
#else
#include "backplane_data.h"
#endif
#include <stdlib.h>
#include <string.h>

/* The backplane cells the video driver shows around the game picture (HDMI,
 * VGA, TV-SOFT; 2x and 3x; BACKPLANE_W x BACKPLANE_H), NULL - none.  The cells are unpacked into RAM:
 * the scanline code must not wait for the flash. */
uint8_t* volatile backplane_cells = NULL;

static uint8_t* cells = NULL;
static int unpacked = -1; // the scale the cells hold

static void unpack(const uint8_t* rle, const size_t size) {
    uint8_t* out = cells;
    uint8_t* const end = cells + BACKPLANE_W * BACKPLANE_H;
    for (size_t i = 0; i + 1 < size && out < end; i += 2) {
        size_t n = rle[i];
        if (n > (size_t)(end - out)) n = end - out;
        memset(out, rle[i + 1], n);
        out += n;
    }
}

/* a backplane loaded from a file: the cells buffer (BACKPLANE_W x BACKPLANE_H)
 * to read colour numbers 0..31 into; not shown meanwhile.  NULL - no room. */
uint8_t* backplane_buffer(void) {
    backplane_cells = NULL;
    if (!cells) cells = (uint8_t *)malloc(BACKPLANE_W * BACKPLANE_H);
    unpacked = -1; // the built-in cells are gone
    return cells;
}

/* show the buffer filled after backplane_buffer() with its 32 colours (R, G, B) */
void backplane_show_loaded(const uint8_t rgb[BACKPLANE_COLORS * 3]) {
    if (!cells) return;
    for (int i = 0; i < BACKPLANE_W * BACKPLANE_H; i++)
        cells[i] = backplane_slot[cells[i] % BACKPLANE_COLORS];
    for (int i = 0; i < BACKPLANE_COLORS; i++)
        graphics_set_palette(backplane_slot[i], (uint32_t)rgb[i * 3] << 16 | rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2]);
    backplane_cells = cells;
}

void graphics_set_backplane(const bool on, const enum graphics_scale_t scale) {
    backplane_cells = NULL; // the video driver stops showing them first
    if (!on || scale == GRAPHICS_SCALE_STRETCH) return;
    if (!cells) {
        cells = (uint8_t *)malloc(BACKPLANE_W * BACKPLANE_H);
        if (!cells) return; // no room (RP2040): no backplane
    }
    if (unpacked != (int)scale) {
        if (scale == GRAPHICS_SCALE_2X) unpack(backplane_rle_2x, sizeof backplane_rle_2x);
        else unpack(backplane_rle_3x, sizeof backplane_rle_3x);
        unpacked = (int)scale;
    }
    for (int i = 0; i < BACKPLANE_COLORS; i++)
        graphics_set_palette(backplane_slot[i], backplane_rgb[i]);
    backplane_cells = cells;
}
