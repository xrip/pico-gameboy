/**
 * Copyright (C) 2023 by Ilya Maslennikov <xrip@xrip.ru>
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#pragma GCC optimize("Ofast")
// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define ENABLE_SDCARD 1
#define USE_PS2_KBD 1
#define USE_NESPAD 1
#define SHOW_FPS 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

/* C Headers */
#include <cstdio>
#include <cstring>

/* RP2040 Headers */
#include "pico/runtime.h"
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>

#if ENABLE_SOUND

#include "audio.h"
#include "minigb_apu.h"

#endif

/* Project headers */
#include "hedley.h"
#include "peanut_gb.h"
#include "gbcolors.h"

/* Murmulator board */
#include "vga.h"
#include "f_util.h"
#include "ff.h"


#if USE_NESPAD

#include "nespad.h"

#endif

#if USE_PS2_KBD

#include "ps2kbd_mrmltr.h"

#endif

#include "VGA_ROM_F16.h"

/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define FLASH_TARGET_OFFSET (1024 * 1024)
const char *rom_filename = (const char *) (XIP_BASE + FLASH_TARGET_OFFSET);
const uint8_t *rom = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET) + 4096;
static unsigned char __attribute__((aligned(4))) rom_bank0[65535];

static uint8_t __attribute__((aligned(4))) ram[32768];

static const sVmode *vmode = nullptr;
struct semaphore vga_start_semaphore;

struct gb_s gb;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
#define TEXTMODE_ROWS 10
#define TEXTMODE_COLS 80
char textmode[TEXTMODE_ROWS][TEXTMODE_COLS];
uint8_t colors[TEXTMODE_ROWS][TEXTMODE_COLS];

static FATFS fs;

#if ENABLE_SOUND
uint16_t *stream;
#endif

#define X2(a) (a | (a << 8))
#define X4(a) (a | (a << 8) | (a << 16) | (a << 24))
#define VGA_RGB_222(r, g, b) ((r << 4) | (g << 2) | b)
#define CHECK_BIT(var, pos) (((var)>>(pos)) & 1)

// Function to convert RGB565 to RGB222
#define convertRGB565toRGB222(color565) \
    (((((color565 >> 11) & 0x1F) * 255 / 31) >> 6) << 4 | \
    ((((color565 >> 5) & 0x3F) * 255 / 63) >> 6) << 2 | \
    ((color565 & 0x1F) * 255 / 31) >> 6)

typedef uint8_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette
static uint8_t manual_palette_selected = 0;

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static input_bits_t keyboard_bits = { true, true, true, true, true, true, true, true };    //Keyboard
static input_bits_t gamepad_bits = { true, true, true, true, true, true, true, true };      //Joypad
//-----------------------------------------------------------------------------
#if USE_NESPAD

void nespad_tick() {
    nespad_read();

    gamepad_bits.a = !(nespad_state & DPAD_A);
    gamepad_bits.b = !(nespad_state & DPAD_B);
    gamepad_bits.select = !(nespad_state & DPAD_SELECT);
    gamepad_bits.start = !(nespad_state & DPAD_START);
    gamepad_bits.up = !(nespad_state & DPAD_UP);
    gamepad_bits.down = !(nespad_state & DPAD_DOWN);
    gamepad_bits.left = !(nespad_state & DPAD_LEFT);
    gamepad_bits.right = !(nespad_state & DPAD_RIGHT);
//-----------------------------------------------------------------------------
}

#endif
//-----------------------------------------------------------------------------


void draw_text(char *string, uint8_t x, uint8_t y, uint8_t color, uint8_t bgcolor) {
    uint8_t len = strlen(string);
    len = len < 80 ? len : 80;
    memcpy(&textmode[y][x], string, len);
    memset(&colors[y][x], (color << 4) | (bgcolor & 0xF), len);
}


#define putstdio(x) write(1, x, strlen(x))

#if USE_PS2_KBD

static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

void __not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
/*    printf("HID key report modifiers %2.2X report ", report->modifier);
    for (unsigned char i: report->keycode)
        printf("%2.2X", i);
    printf("\r\n");*/

    keyboard_bits.start = !isInReport(report, HID_KEY_ENTER);
    keyboard_bits.select = !isInReport(report, HID_KEY_BACKSPACE);
    keyboard_bits.a = !isInReport(report, HID_KEY_Z);
    keyboard_bits.b = !isInReport(report, HID_KEY_X);
    keyboard_bits.up = !isInReport(report, HID_KEY_ARROW_UP);
    keyboard_bits.down = !isInReport(report, HID_KEY_ARROW_DOWN);
    keyboard_bits.left = !isInReport(report, HID_KEY_ARROW_LEFT);
    keyboard_bits.right = !isInReport(report, HID_KEY_ARROW_RIGHT);
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        0,
        process_kbd_report);
#endif

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < sizeof(rom_bank0))
        return rom_bank0[addr];

    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s *gb, const uint_fast32_t addr) {
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void __not_in_flash_func(gb_cart_ram_write)(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr) {
    const char *gb_err_str[4] = {
            "UNKNOWN",
            "INVALID OPCODE",
            "INVALID READ",
            "INVALID WRITE" };
    printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
}

typedef enum {
    RESOLUTION_4X3,
    RESOLUTION_3X3,
    RESOLUTION_TEXTMODE,
} resolution_t;
resolution_t resolution = RESOLUTION_TEXTMODE;

/* Renderer loop on Pico's second core */
void __time_critical_func(render_loop)() {
    multicore_lockout_victim_init();
    VgaLineBuf *linebuf;
    printf("Video on Core#%i running...\n", get_core_num());

    sem_acquire_blocking(&vga_start_semaphore);
    VgaInit(vmode, 640, 480);
    uint8_t pixel;
    uint8_t color;
    uint32_t y;
    while (linebuf = get_vga_line()) {
        y = linebuf->row;

        switch (resolution) {
            case RESOLUTION_3X3:
                if (y >= 8 && y < (8 + LCD_HEIGHT)) {
                    for (int x = 0; x < LCD_WIDTH; x++) {
                        uint16_t x3 = 80 + (x << 1) + x;
                        pixel = SCREEN[y - 8][x];
                        color = palette[(pixel & LCD_PALETTE_ALL) >> 4][pixel & 3];
                        linebuf->line[x3] = color;
                        linebuf->line[x3 + 1] = color;
                        linebuf->line[x3 + 2] = color;
                    }
                } else {
                    memset(linebuf->line, 0, 640);
                }
#if SHOW_FPS
                // SHOW FPS
                if (y < 16) {
                    for (uint8_t x = 77; x < 80; x++) {
                        uint8_t glyph_row = VGA_ROM_F16[(textmode[y / 16][x] * 16) + y % 16];
                        color = colors[y / 16][x];

                        for (uint8_t bit = 0; bit < 8; bit++) {
                            if (CHECK_BIT(glyph_row, bit)) {
                                // FOREGROUND
                                linebuf->line[8 * x + bit] = (color >> 4) & 0xF;
                            } else {
                                // BACKGROUND
                                linebuf->line[8 * x + bit] = color & 0xF;
                            }
                        }
                    }
                }
#endif
                break;
            case RESOLUTION_4X3:
                if (y >= 8 && y < (8 + LCD_HEIGHT)) {
                    for (int x = 0; x < LCD_WIDTH * 4; x += 4) {
                        pixel = SCREEN[(y - 8)][x / 4];
                        (uint32_t &) linebuf->line[x] = X4(palette[(pixel & LCD_PALETTE_ALL) >> 4][pixel & 3]);
                    }
                } else {
                    memset(linebuf->line, 0, 640);
                }
                break;

            case RESOLUTION_TEXTMODE:
                for (uint8_t x = 0; x < 80; x++) {
                    uint8_t glyph_row = VGA_ROM_F16[(textmode[y / 16][x] * 16) + y % 16];
                    color = colors[y / 16][x];

                    for (uint8_t bit = 0; bit < 8; bit++) {
                        if (CHECK_BIT(glyph_row, bit)) {
                            // FOREGROUND
                            linebuf->line[8 * x + bit] = (color >> 4) & 0xF;
                        } else {
                            // BACKGROUND
                            linebuf->line[8 * x + bit] = color & 0xF;
                        }
                    }
                }
                break;
        }
    }

    HEDLEY_UNREACHABLE();
}

#if ENABLE_LCD

/**
 * Draws scanline into framebuffer.
 */
void __always_inline lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t y) {
    memcpy((uint32_t *) SCREEN[y], (uint32_t *) pixels, 160);
//         screen[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    //for (unsigned int x = 0; x < LCD_WIDTH; x++)
//        screen[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
}

#endif

#if ENABLE_SDCARD

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s *gb) {
    char filename[16];
    uint_fast32_t save_size;
    UINT br;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {

        FRESULT fr = f_mount(&fs, "", 1);
        if (FR_OK != fr) {
            printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_READ);
        if (fr == FR_OK) {
            f_read(&fil, ram, f_size(&fil), &br);
        } else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I read_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s *gb) {
    char filename[16];
    uint_fast32_t save_size;
    UINT bw;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {

        FRESULT fr = f_mount(&fs, "", 1);
        if (FR_OK != fr) {
            printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) {
            f_write(&fil, ram, save_size, &bw);
        } else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I write_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}

void  fileselector_load(char *pathname) {
    if (strcmp(rom_filename, pathname) == 0) {
        printf("Launching last rom");
        return;
    }

    FIL fil;
    FRESULT fr;

    size_t bufsize = sizeof(ram);
    BYTE *buffer = (BYTE *) ram;
    auto ofs = FLASH_TARGET_OFFSET;
    printf("Writing %s rom to flash %x\r\n", pathname, ofs);
    fr = f_open(&fil, pathname, FA_READ);

    UINT bytesRead;
    if (fr == FR_OK) {
        uint32_t ints = save_and_disable_interrupts();
        multicore_lockout_start_blocking();

        // TODO: Save it after success loading to prevent corruptions
        printf("Flashing %d bytes to flash address %x\r\n", 256, ofs);
        flash_range_erase(ofs, 4096);
        flash_range_program(ofs, reinterpret_cast<const uint8_t *>(pathname), 256);

        ofs += 4096;
        for (;;) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            fr = f_read(&fil, buffer, bufsize, &bytesRead);
            if (fr == FR_OK) {
                if (bytesRead == 0) {
                    break;
                }

                printf("Flashing %d bytes to flash address %x\r\n", bytesRead, ofs);

                printf("Erasing...");
                gpio_put(PICO_DEFAULT_LED_PIN, false);
                // Disable interupts, erase, flash and enable interrupts
                flash_range_erase(ofs, bufsize);
                printf("  -> Flashing...\r\n");
                flash_range_program(ofs, buffer, bufsize);

                ofs += bufsize;
            } else {
                printf("Error reading rom: %d\n", fr);
                break;
            }
        }


        f_close(&fil);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
    }
}

/**
 * Function used by the rom file selector to display one page of .gb rom files
 */
uint16_t fileselector_display_page(char filenames[28][256], uint16_t page_number) {
#define ROWS TEXTMODE_ROWS-1
    // Dirty screen cleanup
    memset(&textmode, 0x00, sizeof(textmode));
    memset(&colors, 0x00, sizeof(colors));
    char footer[80];
    sprintf(footer, "=================== PAGE #%i -> NEXT PAGE / <- PREV. PAGE ====================", page_number);
    draw_text(footer, 0, ROWS, 3, 11);

    DIR directory;
    FILINFO file;
    FRESULT result;

    result = f_mount(&fs, "", 1);
    if (FR_OK != result) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(result), result);
        return 0;
    }

    /* clear the filenames array */
    for (uint8_t ifile = 0; ifile < (TEXTMODE_ROWS - 1); ifile++) {
        strcpy(filenames[ifile], "");
    }

    uint16_t total_files = 0;
    result = f_findfirst(&directory, &file, "GB\\", "*.gb");

    /* skip the first N pages */
    if (page_number > 0) {
        while (total_files < page_number * ROWS && result == FR_OK && file.fname[0]) {
            total_files++;
            result = f_findnext(&directory, &file);
        }
    }

    /* store the filenames of this page */
    total_files = 0;
    while (total_files < ROWS && result == FR_OK && file.fname[0]) {
        strcpy(filenames[total_files], file.fname);
        total_files++;
        result = f_findnext(&directory, &file);
    }
    f_closedir(&directory);

    for (uint8_t ifile = 0; ifile < total_files; ifile++) {
        char pathname[255];
        uint8_t color = 0x0d;
        sprintf(pathname, "GB\\%s", filenames[ifile]);

        if (strcmp(pathname, rom_filename) != 0) {
            color = 0xFF;
        }
        draw_text(filenames[ifile], 0, ifile, color, 0x00);
    }
    return total_files;
}

/**
 * The ROM selector displays pages of up to 22 rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the root directory of the SD card
 */
void fileselector() {
    uint16_t num_page = 0;
    char filenames[30][256];

    printf("Selecting ROM\r\n");

    /* display the first page with up to 22 rom files */
    uint16_t numfiles = fileselector_display_page(filenames, num_page);

    /* select the first rom */
    uint8_t current_file = 0;
    uint8_t color = 0xFF;
    draw_text(filenames[current_file], 0, current_file, 0xFF, 0xF8);

    while (true) {
        char pathname[255];
        sprintf(pathname, "GB\\%s", filenames[current_file]);

        if (strcmp(pathname, rom_filename) != 0) {
            color = 0xFF;
        } else {
            color = 0x0d;
        }
#if USE_PS2_KBD
        ps2kbd.tick();
#endif

#if USE_NESPAD
        nespad_tick();
#endif
        sleep_ms(33);
#if USE_NESPAD
        nespad_tick();
#endif
//-----------------------------------------------------------------------------
        gamepad_bits.up = keyboard_bits.up && gamepad_bits.up;
        gamepad_bits.down = keyboard_bits.down && gamepad_bits.down;
        gamepad_bits.left = keyboard_bits.left && gamepad_bits.left;
        gamepad_bits.right = keyboard_bits.right && gamepad_bits.right;
        gamepad_bits.a = keyboard_bits.a && gamepad_bits.a;
        gamepad_bits.b = keyboard_bits.b && gamepad_bits.b;
        gamepad_bits.select = keyboard_bits.select && gamepad_bits.select;
        gamepad_bits.start = keyboard_bits.start && gamepad_bits.start;
//-----------------------------------------------------------------------------
        if (!gamepad_bits.start || !gamepad_bits.a || !gamepad_bits.b) {
            /* copy the rom from the SD card to flash and start the game */
            fileselector_load(pathname);
            break;
        }
        if (!gamepad_bits.down) {
            /* select the next rom */
            draw_text(filenames[current_file], 0, current_file, color, 0x00);
            current_file++;
            if (current_file >= numfiles)
                current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 0xF8);
            sleep_ms(150);
        }
        if (!gamepad_bits.up) {
            /* select the previous rom */
            draw_text(filenames[current_file], 0, current_file, color, 0x00);
            if (current_file == 0) {
                current_file = numfiles - 1;
            } else {
                current_file--;
            }
            draw_text(filenames[current_file], 0, current_file, color, 0xF8);
            sleep_ms(150);
        }
        if (!gamepad_bits.right) {
            /* select the next page */
            num_page++;
            numfiles = fileselector_display_page(filenames, num_page);
            if (numfiles == 0) {
                /* no files in this page, go to the previous page */
                num_page--;
                numfiles = fileselector_display_page(filenames, num_page);
            }
            /* select the first file */
            current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 0xF8);
            sleep_ms(150);
        }
        if ((!gamepad_bits.left) && num_page > 0) {
            /* select the previous page */
            num_page--;
            numfiles = fileselector_display_page(filenames, num_page);
            /* select the first file */
            current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 0xF8);
            sleep_ms(150);
        }
        tight_loop_contents();
    }
}

#endif


#define MENU_ITEMS_NUMBER 5
#if MENU_ITEMS_NUMBER > TEXTMODE_ROWS
error("Too much menu items!")
#endif
const char menu_items[MENU_ITEMS_NUMBER][80] = {
        { "Frameskip %i  " },
        { "Palette %i  " },
        { "Resolution Scale %i  " },
        { "Reset to ROM select" },
        { "Return to %s" },
};

bool restart = false;
uint8_t frameskip = gb.direct.frame_skip;
resolution_t old_resolution;

void *menu_values[MENU_ITEMS_NUMBER] = {
        &frameskip,
        &manual_palette_selected,
        &old_resolution,
        nullptr,
        (void *) rom_filename,
};

void menu() {
    bool exit = false;
    old_resolution = resolution;
    memset(&textmode, 0x00, sizeof(textmode));
    memset(&colors, 0x00, sizeof(colors));
    resolution = RESOLUTION_TEXTMODE;

    int current_item = 0;
    char item[80];

    while (!exit) {
#if USE_PS2_KBD
        ps2kbd.tick();
#endif
#if USE_NESPAD
        nespad_tick();
#endif
        sleep_ms(25);
#if USE_NESPAD
        nespad_tick();
#endif
        gamepad_bits.up = keyboard_bits.up && gamepad_bits.up;
        gamepad_bits.down = keyboard_bits.down && gamepad_bits.down;
        gamepad_bits.left = keyboard_bits.left && gamepad_bits.left;
        gamepad_bits.right = keyboard_bits.right && gamepad_bits.right;
        gamepad_bits.start = keyboard_bits.start && gamepad_bits.start;
        gamepad_bits.a = keyboard_bits.a && gamepad_bits.a;
        gamepad_bits.b = keyboard_bits.b && gamepad_bits.b;

        if (!gamepad_bits.down) {
            if (current_item < MENU_ITEMS_NUMBER - 1) {
                current_item++;
            } else {
                current_item = 0;
            }
        }

        if (!gamepad_bits.up) {
            if (current_item > 0) {
                current_item--;
            } else {
                current_item = MENU_ITEMS_NUMBER - 1;
            }
        }

        if (!gamepad_bits.left || !gamepad_bits.right) {
            switch (current_item) {
                case 0:  // Frameskip
                    frameskip = gb.direct.frame_skip = !gb.direct.frame_skip;
                    break;
                case 1:  // Palette
                    if (!gamepad_bits.right) {
                        manual_palette_selected = (manual_palette_selected + 1) % 13;
                    } else if (manual_palette_selected > 0) {
                        manual_palette_selected -= 1;
                    }
                    manual_assign_palette(palette16, manual_palette_selected);
                    for (int i = 0; i < 3; i++)
                        for (int j = 0; j < 4; j++)
                            palette[i][j] = convertRGB565toRGB222(palette16[i][j]);
                    break;
                case 2:  // Resolution
                    if (!gamepad_bits.right) {
                        old_resolution = static_cast<resolution_t>((old_resolution + 1) % 2);
                    } else if (old_resolution > 0) {
                        old_resolution = static_cast<resolution_t>((old_resolution - 1));
                    }
                    break;
            }
        }

        if (!gamepad_bits.start || !gamepad_bits.a || !gamepad_bits.b) {
            switch (current_item) {
                case MENU_ITEMS_NUMBER - 2:
#if ENABLE_SDCARD
                    write_cart_ram_file(&gb);
#endif
                    restart = true;
                case MENU_ITEMS_NUMBER - 1:
                    exit = true;
                    break;
            }
        }

        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            // TODO: textmode maxy from define
            uint8_t y = i + ((TEXTMODE_ROWS - MENU_ITEMS_NUMBER) >> 1);
            uint8_t x = 30;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            if (strstr(menu_items[i], "%s") != nullptr) {
                sprintf(item, menu_items[i], menu_values[i]);
            } else {
                sprintf(item, menu_items[i], *(uint8_t *) menu_values[i]);
            }
            draw_text(item, x, y, color, bg_color);
        }

        sleep_ms(100);
    }

    resolution = old_resolution;
}

int main() {
    /* Overclock. */
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    set_sys_clock_khz(288000, true);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

#if !NDEBUG
    stdio_init_all();
#endif

    putstdio("INIT: ");

    printf("VGA ");
    sleep_ms(50);
    vmode = Video(DEV_VGA, RES_HVGA);   //R
    sleep_ms(50);

#if USE_PS2_KBD
    printf("PS2 KBD ");
    ps2kbd.init_gpio();
#endif

#if USE_NESPAD
    printf("NESPAD %i", nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT));
#endif

#if ENABLE_SOUND
    printf("SOUND ");

    // Allocate memory for the stream buffer
    stream = static_cast<uint16_t *>(malloc(AUDIO_BUFFER_SIZE_BYTES));
    assert(stream != NULL);
    memset(stream, 0, AUDIO_BUFFER_SIZE_BYTES);  // Zero out the stream buffer

    // Initialize I2S sound driver
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
#endif


    /* Start Core1, which processes requests to the LCD. */
    putstdio("CORE1 ");

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_loop);
    sem_release(&vga_start_semaphore);

#if ENABLE_SOUND
    // Initialize audio emulation
    audio_init();

    putstdio("AUDIO ");
#endif

//while (1) {}

    while (true) {
#if ENABLE_LCD
#if ENABLE_SDCARD
        /* ROM File selector */
        resolution = RESOLUTION_TEXTMODE;
        fileselector();
#endif
#endif
        resolution = RESOLUTION_3X3;

        /* Initialise GB context. */
        memcpy(rom_bank0, rom, sizeof(rom_bank0));
        enum gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                           &gb_cart_ram_write, &gb_error, nullptr);
        putstdio("GB ");

        if (ret != GB_INIT_NO_ERROR) {
            printf("Error: %d\n", ret);
            break;
        }

        /* Automatically assign a colour palette to the game */
        if (!manual_palette_selected) {
            char rom_title[16];
            auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
        } else {
            manual_assign_palette(palette16, manual_palette_selected);
        }

        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                palette[i][j] = convertRGB565toRGB222(palette16[i][j]);

#if ENABLE_LCD
        gb_init_lcd(&gb, &lcd_draw_line);
        putstdio("LCD ");
#endif

#if ENABLE_SDCARD
        /* Load Save File. */
        read_cart_ram_file(&gb);
#endif

        putstdio("\n> ");
        uint_fast32_t frames = 0;
        uint64_t start_time = time_us_64();

//=============================================================================
        while (!restart) {
#if USE_PS2_KBD
            ps2kbd.tick();
#endif
            //-----------------------------------------------------------------
            gb_run_frame(&gb);

            frames++;

#if ENABLE_SOUND
            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
#endif
#if SHOW_FPS
            if (frames == 60) {
                uint64_t end_time;
                uint32_t diff;
                uint8_t fps;

                end_time = time_us_64();
                diff = end_time - start_time;
                fps = ((uint64_t) frames * 1000 * 1000) / diff;
                char fps_text[4];
                sprintf(fps_text, "%i ", fps);
                draw_text(fps_text, 77, 0, 15, 0);
                frames = 0;
                start_time = time_us_64();
            }
#endif

#if USE_NESPAD
            nespad_tick();
#endif
//------------------------------------------------------------------------------
            gb.direct.joypad_bits.up = keyboard_bits.up && gamepad_bits.up;
            gb.direct.joypad_bits.down = keyboard_bits.down && gamepad_bits.down;
            gb.direct.joypad_bits.left = keyboard_bits.left && gamepad_bits.left;
            gb.direct.joypad_bits.right = keyboard_bits.right && gamepad_bits.right;
            gb.direct.joypad_bits.a = keyboard_bits.a && gamepad_bits.a;
            gb.direct.joypad_bits.b = keyboard_bits.b && gamepad_bits.b;
            gb.direct.joypad_bits.select = keyboard_bits.select && gamepad_bits.select;
            gb.direct.joypad_bits.start = keyboard_bits.start && gamepad_bits.start;
//------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            if (!gb.direct.joypad_bits.select && !gb.direct.joypad_bits.start) {
                menu();
                continue;
            }
        }
        restart = false;
    }

}
