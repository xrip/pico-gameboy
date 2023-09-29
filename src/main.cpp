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

#define PEANUT_GB_HIGH_LCD_ACCURACY 1

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
const uint8_t *rom = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);
static unsigned char rom_bank0[65535];

static uint8_t ram[32768];

static const sVmode *vmode = nullptr;
struct semaphore vga_start_semaphore;

struct gb_s gb;

uint8_t screen[LCD_HEIGHT][LCD_WIDTH];
char textmode[30][80];
uint8_t colors[30][80];

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

struct joypad_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static joypad_bits_t keyboard_bits = { true, true, true, true, true, true, true, true };    //Keyboard
static joypad_bits_t joypad_bits = { true, true, true, true, true, true, true, true };      //Joypad
static joypad_bits_t prev_joypad_bits = { true, true, true, true, true, true, true, true };
//-----------------------------------------------------------------------------
#if USE_NESPAD

void nespad_tick() {
    nespad_read();
//-----------------------------------------------------------------------------
    if (nespad_state & 0x01) { joypad_bits.a = false; } else { joypad_bits.a = true; }
    if (nespad_state & 0x02) { joypad_bits.b = false; } else { joypad_bits.b = true; }
    if (nespad_state & 0x04) { joypad_bits.select = false; } else { joypad_bits.select = true; }
    if (nespad_state & 0x08) { joypad_bits.start = false; } else { joypad_bits.start = true; }
    if (nespad_state & 0x10) { joypad_bits.up = false; } else { joypad_bits.up = true; }
    if (nespad_state & 0x20) { joypad_bits.down = false; } else { joypad_bits.down = true; }
    if (nespad_state & 0x40) { joypad_bits.left = false; } else { joypad_bits.left = true; }
    if (nespad_state & 0x80) { joypad_bits.right = false; } else { joypad_bits.right = true; }
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

    //-------------------------------------------------------------------------
    if (isInReport(report, 0x28)) { keyboard_bits.start = false; } else { keyboard_bits.start = true; }
    if (isInReport(report, 0x2A)) { keyboard_bits.select = false; } else { keyboard_bits.select = true; }
    if (isInReport(report, 0x1D)) { keyboard_bits.a = false; } else { keyboard_bits.a = true; }
    if (isInReport(report, 0x1B)) { keyboard_bits.b = false; } else { keyboard_bits.b = true; }
    if (isInReport(report, 0x52)) { keyboard_bits.up = false; } else { keyboard_bits.up = true; }
    if (isInReport(report, 0x51)) { keyboard_bits.down = false; } else { keyboard_bits.down = true; }
    if (isInReport(report, 0x50)) { keyboard_bits.left = false; } else { keyboard_bits.left = true; }
    if (isInReport(report, 0x4F)) { keyboard_bits.right = false; } else { keyboard_bits.right = true; }
    //-------------------------------------------------------------------------
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        0,
        process_kbd_report);
#endif

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void) gb;
    if (addr < sizeof(rom_bank0))
        return rom_bank0[addr];

    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void) gb;
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
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
    RESOLUTION_2X2,
    RESOLUTION_TEXTMODE,
    RESOLUTION_NATIVE,
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
            case RESOLUTION_4X3:
                if (y >= 24 && y < (24 + LCD_HEIGHT * 3)) {
                    for (int x = 0; x < LCD_WIDTH * 4; x += 4) {
                        pixel = screen[(y - 24) / 3][x / 4];
                        (uint32_t &) linebuf->line[x] = X4(palette[(pixel & LCD_PALETTE_ALL) >> 4][pixel & 3]);
                    }
                } else {
                    memset(linebuf->line, 0, 640);
                }
                break;
            case RESOLUTION_3X3:
                if (y >= 24 && y < (24 + LCD_HEIGHT * 3)) {
                    for (int x = 0; x < LCD_WIDTH; x++) {
                        uint16_t x3 = 80 + (x <<  1) + x;
                        pixel = screen[(y - 24) / 3][x];
                        color = palette[(pixel & LCD_PALETTE_ALL) >> 4][pixel & 3];
                        linebuf->line[x3] = color;
                        linebuf->line[x3 + 1] = color;
                        linebuf->line[x3 + 2] = color;
                    }
                } else {
                    memset(linebuf->line, 0, 640);
                }
                break;
            case RESOLUTION_2X2:
                //if (y >= 48 && y < 48 + LCD_HEIGHT) {
                if (y >= 96 && y < (96 + LCD_HEIGHT * 2)) {
                    for (int x = 0; x < LCD_WIDTH * 2; x += 2) {
                         pixel = screen[(y - 96) / 2][x / 2];
                        (uint16_t &) linebuf->line[160 + x] = X2(palette[(pixel & LCD_PALETTE_ALL) >> 4][pixel & 3]);
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
            case RESOLUTION_NATIVE:
                if (y >= 168 && y < 168 + LCD_HEIGHT) {
                    memcpy(&linebuf->line[240], &screen[y - 168][0], LCD_WIDTH);
                } else {
                    memset(linebuf->line, 0, 640);
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
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t y) {
    memcpy((uint32_t *)screen[y], (uint32_t *)pixels, 160);
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

/**
 * Load a .gb rom file in flash from the SD card
 */
void load_cart_rom_file(char *filename) {
    UINT br = 0;
    uint8_t buffer[FLASH_SECTOR_SIZE];
    bool mismatch = false;

    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    FIL fil;
    fr = f_open(&fil, filename, FA_READ);
    if (fr == FR_OK) {
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
        for (;;) {
            f_read(&fil, buffer, sizeof buffer, &br);
            if (br == 0)
                break; /* end of file */

            printf("I Erasing target region...\n");
            flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
            printf("I Programming target region...\n");
            flash_range_program(flash_target_offset, buffer, FLASH_SECTOR_SIZE);
            /* Read back target region and check programming */
            printf("I Done. Reading back target region...\n");
            for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
                if (rom[flash_target_offset + i] != buffer[i]) {
                    mismatch = true;
                }
            }

            /* Next sector */
            flash_target_offset += FLASH_SECTOR_SIZE;
        }
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
        if (mismatch) {
            printf("I Programming successful!\n");
        } else {
            printf("E Programming failed!\n");
        }
    } else {
        printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
    }

    fr = f_close(&fil);
    if (fr != FR_OK) {
        printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }

    printf("I load_cart_rom_file(%s) COMPLETE (%u bytes)\n", filename, br);
}

/**
 * Function used by the rom file selector to display one page of .gb rom files
 */
uint16_t rom_file_selector_display_page(char filename[28][256], uint16_t num_page) {
    // Dirty screen cleanup
    memset(&textmode, 0x00, sizeof(textmode));
    memset(&colors, 0x00, sizeof(colors));
    char footer[80];
    sprintf(footer, "=================== PAGE #%i -> NEXT PAGE / <- PREV. PAGE ====================", num_page);
    draw_text(footer, 0, 29, 3, 11);

    DIR dj;
    FILINFO fno;
    FRESULT fr;

    fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return 0;
    }

    /* clear the filenames array */
    for (uint8_t ifile = 0; ifile < 28; ifile++) {
        strcpy(filename[ifile], "");
    }

    /* search *.gb files */
    uint16_t num_file = 0;
    fr = f_findfirst(&dj, &fno, "GB\\", "*.gb");

    /* skip the first N pages */
    if (num_page > 0) {
        while (num_file < num_page * 28 && fr == FR_OK && fno.fname[0]) {
            num_file++;
            fr = f_findnext(&dj, &fno);
        }
    }

    /* store the filenames of this page */
    num_file = 0;
    while (num_file < 28 && fr == FR_OK && fno.fname[0]) {
        strcpy(filename[num_file], fno.fname);
        num_file++;
        fr = f_findnext(&dj, &fno);
    }
    f_closedir(&dj);

    /* display *.gb rom files on screen */
    // mk_ili9225_fill(0x0000);
    for (uint8_t ifile = 0; ifile < num_file; ifile++) {
        draw_text(filename[ifile], 0, ifile, 0xFF, 0x00);
    }
    return num_file;
}

/**
 * The ROM selector displays pages of up to 22 rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the root directory of the SD card
 */
void rom_file_selector() {
    uint16_t num_page = 0;
    char filenames[30][256];

    printf("Selecting ROM\r\n");

    /* display the first page with up to 22 rom files */
    uint16_t numfiles = rom_file_selector_display_page(filenames, num_page);

    /* select the first rom */
    uint8_t selected = 0;
    draw_text(filenames[selected], 0, selected, 0xFF, 0xF8);

    while (true) {

#if USE_PS2_KBD
        ps2kbd.tick();
#endif

#if USE_NESPAD
        nespad_tick();
#endif
//-----------------------------------------------------------------------------
        joypad_bits.up = keyboard_bits.up && joypad_bits.up;
        joypad_bits.down = keyboard_bits.down && joypad_bits.down;
        joypad_bits.left = keyboard_bits.left && joypad_bits.left;
        joypad_bits.right = keyboard_bits.right && joypad_bits.right;
        joypad_bits.a = keyboard_bits.a && joypad_bits.a;
        joypad_bits.b = keyboard_bits.b && joypad_bits.b;
        joypad_bits.select = keyboard_bits.select && joypad_bits.select;
        joypad_bits.start = keyboard_bits.start && joypad_bits.start;
//-----------------------------------------------------------------------------
        if (!joypad_bits.start || !joypad_bits.a || !joypad_bits.b) {
            /* copy the rom from the SD card to flash and start the game */
            char pathname[255];
            sprintf(pathname, "GB\\%s", filenames[selected]);
            load_cart_rom_file(pathname);
            break;
        }
        if (!joypad_bits.down) {
            /* select the next rom */
            draw_text(filenames[selected], 0, selected, 0xFF, 0x00);
            selected++;
            if (selected >= numfiles)
                selected = 0;
            draw_text(filenames[selected], 0, selected, 0xFF, 0xF8);
            sleep_ms(150);
        }
        if (!joypad_bits.up) {
            /* select the previous rom */
            draw_text(filenames[selected], 0, selected, 0xFF, 0x00);
            if (selected == 0) {
                selected = numfiles - 1;
            } else {
                selected--;
            }
            draw_text(filenames[selected], 0, selected, 0xFF, 0xF8);
            sleep_ms(150);
        }
        if (!joypad_bits.right) {
            /* select the next page */
            num_page++;
            numfiles = rom_file_selector_display_page(filenames, num_page);
            if (numfiles == 0) {
                /* no files in this page, go to the previous page */
                num_page--;
                numfiles = rom_file_selector_display_page(filenames, num_page);
            }
            /* select the first file */
            selected = 0;
            draw_text(filenames[selected], 0, selected, 0xFF, 0xF8);
            sleep_ms(150);
        }
        if ((!joypad_bits.left) && num_page > 0) {
            /* select the previous page */
            num_page--;
            numfiles = rom_file_selector_display_page(filenames, num_page);
            /* select the first file */
            selected = 0;
            draw_text(filenames[selected], 0, selected, 0xFF, 0xF8);
            sleep_ms(150);
        }
        tight_loop_contents();
    }
}

#endif

int main() {
    enum gb_init_error_e ret;

    /* Overclock. */
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    set_sys_clock_khz(288000, true);

    /* Initialise USB serial connection for debugging. */
    stdio_init_all();
    // time_init();
    //sleep_ms(5000);

    putstdio("INIT: ");

    printf("VGA ");
    sleep_ms(50);
    vmode = Video(DEV_VGA, RES_VGA);   //R
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
        bool restart = false;

#if ENABLE_LCD
#if ENABLE_SDCARD
        /* ROM File selector */
        resolution = RESOLUTION_TEXTMODE;
        rom_file_selector();
#endif
#endif
        resolution = RESOLUTION_3X3;

        /* Initialise GB context. */
        memcpy(rom_bank0, rom, sizeof(rom_bank0));
        ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                      &gb_cart_ram_write, &gb_error, nullptr);
        putstdio("GB ");

        if (ret != GB_INIT_NO_ERROR) {
            printf("Error: %d\n", ret);
            break;
        }

        /* Automatically assign a colour palette to the game */
        char rom_title[16];
        auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
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
            int input;
#if USE_PS2_KBD
            ps2kbd.tick();
#endif
            //-----------------------------------------------------------------
            gb.gb_frame = 0;

            do {
                __gb_step_cpu(&gb);
                tight_loop_contents();
            } while (HEDLEY_LIKELY(gb.gb_frame == 0));

            frames++;

#if ENABLE_SOUND
            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
#endif
//------------------------------------------------------------------------
            /* Update buttons state */
            prev_joypad_bits.up = gb.direct.joypad_bits.up;
            prev_joypad_bits.down = gb.direct.joypad_bits.down;
            prev_joypad_bits.left = gb.direct.joypad_bits.left;
            prev_joypad_bits.right = gb.direct.joypad_bits.right;
            prev_joypad_bits.a = gb.direct.joypad_bits.a;
            prev_joypad_bits.b = gb.direct.joypad_bits.b;
            prev_joypad_bits.select = gb.direct.joypad_bits.select;
            prev_joypad_bits.start = gb.direct.joypad_bits.start;

#if USE_NESPAD
            nespad_tick();
#endif
//------------------------------------------------------------------------------
            gb.direct.joypad_bits.up = keyboard_bits.up && joypad_bits.up;
            gb.direct.joypad_bits.down = keyboard_bits.down && joypad_bits.down;
            gb.direct.joypad_bits.left = keyboard_bits.left && joypad_bits.left;
            gb.direct.joypad_bits.right = keyboard_bits.right && joypad_bits.right;
            gb.direct.joypad_bits.a = keyboard_bits.a && joypad_bits.a;
            gb.direct.joypad_bits.b = keyboard_bits.b && joypad_bits.b;
            gb.direct.joypad_bits.select = keyboard_bits.select && joypad_bits.select;
            gb.direct.joypad_bits.start = keyboard_bits.start && joypad_bits.start;
//------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            if (!gb.direct.joypad_bits.select) {
#if ENABLE_SOUND
                if (!gb.direct.joypad_bits.up && prev_joypad_bits.up) {
                    /* select + up: increase sound volume */
                    // i2s_increase_volume(&i2s_config);
                }
                if (!gb.direct.joypad_bits.down && prev_joypad_bits.down) {
                    /* select + down: decrease sound volume */
                    // i2s_decrease_volume(&i2s_config);
                }
#endif
                if (!gb.direct.joypad_bits.up && prev_joypad_bits.up) {
                    resolution = static_cast<resolution_t>((resolution + 1) % 3);
                }
                if (!gb.direct.joypad_bits.down && prev_joypad_bits.down) {
                    resolution = static_cast<resolution_t>((resolution - 1) % 3);
                }
                if (!gb.direct.joypad_bits.right && prev_joypad_bits.right) {
                    /* select + right: select the next manual color palette */
                    if (manual_palette_selected < 12) {
                        manual_palette_selected++;
                        manual_assign_palette(palette16, manual_palette_selected);
                        for (int i = 0; i < 3; i++)
                            for (int j = 0; j < 4; j++)
                                palette[i][j] = convertRGB565toRGB222(palette16[i][j]);
                    }
                }
                if (!gb.direct.joypad_bits.left && prev_joypad_bits.left) {
                    /* select + left: select the previous manual color palette */
                    if (manual_palette_selected > 0) {
                        manual_palette_selected--;
                        manual_assign_palette(palette16, manual_palette_selected);
                        for (int i = 0; i < 3; i++)
                            for (int j = 0; j < 4; j++)
                                palette[i][j] = convertRGB565toRGB222(palette16[i][j]);
                    }
                }
                if (!gb.direct.joypad_bits.a && prev_joypad_bits.a) {
                    /* select + A: enable/disable frame-skip => fast-forward */
                    gb.direct.frame_skip = !gb.direct.frame_skip;
                    printf("I gb.direct.frame_skip = %d\n", gb.direct.frame_skip);
                }

                // Restart button
                if (!gb.direct.joypad_bits.b && prev_joypad_bits.b) {
#if ENABLE_SDCARD
                    write_cart_ram_file(&gb);
#endif
                    restart = true;
                }
            }

            /* Serial monitor commands */
            input = getchar_timeout_us(0);
            if (input == PICO_ERROR_TIMEOUT)
                continue;

            switch (input) {
#if 0
                static bool invert = false;
                static bool sleep = false;
                static uint8_t freq = 1;
                static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

                case 'i':
                    invert = !invert;
                    mk_ili9225_display_control(invert, colour);
                    break;

                case 'f':
                    freq++;
                    freq &= 0x0F;
                    mk_ili9225_set_drive_freq(freq);
                    printf("Freq %u\n", freq);
                    break;
#endif
                case 'i':
                    gb.direct.interlace = !gb.direct.interlace;
                    break;

                case 'f':
                    gb.direct.frame_skip = !gb.direct.frame_skip;
                    break;

                case 'b': {
                    uint64_t end_time;
                    uint32_t diff;
                    uint32_t fps;

                    end_time = time_us_64();
                    diff = end_time - start_time;
                    fps = ((uint64_t) frames * 1000 * 1000) / diff;
                    printf("Frames: %u\r\n"
                           "Time: %lu us\r\n"
                           "FPS: %lu\r\n",
                           frames, diff, fps);
                    stdio_flush();
                    frames = 0;
                    start_time = time_us_64();
                    break;
                }

                case '\n':
                case '\r': {
                    gb.direct.joypad_bits.start = 0;
                    break;
                }

                case '\b': {
                    gb.direct.joypad_bits.select = 0;
                    break;
                }

                case '8': {
                    gb.direct.joypad_bits.up = 0;
                    break;
                }

                case '2': {
                    gb.direct.joypad_bits.down = 0;
                    break;
                }

                case '4': {
                    gb.direct.joypad_bits.left = 0;
                    break;
                }

                case '6': {
                    gb.direct.joypad_bits.right = 0;
                    break;
                }

                case 'z':
                case 'w': {
                    gb.direct.joypad_bits.a = 0;
                    break;
                }

                case 'x': {
                    gb.direct.joypad_bits.b = 0;
                    break;
                }

                default:
                    break;
            }
        }
        puts("\nEmulation Ended");
    }
}
