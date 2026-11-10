#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stdio.h"
#include "stdint.h"

#ifdef TFT
#include "st7789.h"
#endif
#ifdef HDMI
#include "hdmi.h"
#endif
#ifdef VGA
#include "vga.h"
#endif
#ifdef TV
#include "tv.h"
#endif
#ifdef SOFTTV
#include "tv-software.h"
#endif


#include "font6x8.h"
#include "font8x8.h"
#include "font8x16.h"

enum graphics_mode_t {
    TEXTMODE_DEFAULT,
    GRAPHICSMODE_DEFAULT,

    TEXTMODE_53x30,

    TEXTMODE_160x100,

    CGA_160x200x16,
    CGA_320x200x4,
    CGA_640x200x2,

    TGA_320x200x16,
    EGA_320x200x16x4,
    VGA_320x240x256,
    VGA_320x200x256x4,
    // planar VGA
};

// Буффер текстового режима
extern uint8_t* text_buffer;

void graphics_init();

void graphics_set_mode(enum graphics_mode_t mode);

void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height);

#if VGA || HDMI || SOFTTV
/* Bottom-of-screen Demo OSD, drawn by scanout (not into the game frame).
 * 255 is the black screen border (graphics_set_bgcolor), 215 is an existing
 * high-contrast colour (white on HDMI/TV, bright on VGA).  Backplane owns
 * 192..199 and 216..239: never overwrite those palette slots for OSD. */
#define GRAPHICS_OSD_BG 255u
#define GRAPHICS_OSD_FG 215u
void graphics_set_overlay(const char* text, bool enabled);
#endif

/* HDMI / VGA / TV: counts the ends of the picture (once per field).  A buffer
 * set right after it (before the vertical blank is over) is shown whole from
 * the next field; the palette entries changed then apply to it alone. */
extern volatile uint32_t graphics_frame_count;

void graphics_set_offset(int x, int y);

// HDMI / VGA / TV-SOFT, GRAPHICSMODE_DEFAULT: Game Boy picture scale on the screen
enum graphics_scale_t {
    GRAPHICS_SCALE_2X,      // 2x2 physical pixels per buffer pixel
    GRAPHICS_SCALE_3X,      // 3x3 (HDMI: 3 lines, 4/2 pixels in turn - 2-pixel steps)
    GRAPHICS_SCALE_STRETCH, // the whole 640x480 screen
};
void graphics_set_scale(enum graphics_scale_t scale);

/* HDMI / VGA / TV-SOFT: the Game Boy Color picture around the game (2x and
 * 3x; Full has none).  backplane_cells: 320x240 cells of 2x2 pixels of the
 * 640x480 screen (a TV field line is a cell row), video palette slots.
 * TV-SOFT: 384 cells across - a PAL field line is wider than 4:3 of its 240
 * lines (~383 cells, NTSC ~321). */
#if SOFTTV
#define BACKPLANE_W 384
#else
#define BACKPLANE_W 320
#endif
#define BACKPLANE_H 240
extern uint8_t* volatile backplane_cells;
void graphics_set_backplane(bool on, enum graphics_scale_t scale); // the built-in one
/* a loaded one: fill backplane_buffer() with colour numbers 0..31, then show it */
#define BACKPLANE_LOADED_COLORS 32
uint8_t* backplane_buffer(void);
void backplane_show_loaded(const uint8_t rgb[BACKPLANE_LOADED_COLORS * 3]);

void graphics_set_palette(uint8_t i, uint32_t color);

void graphics_set_textbuffer(uint8_t* buffer);

void graphics_set_bgcolor(uint32_t color888);

void graphics_set_flashmode(bool flash_line, bool flash_frame);

void draw_text(const char *string, uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor);
void draw_window(const char *title, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void clrScr(uint8_t color);

#ifdef __cplusplus
}
#endif
