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
