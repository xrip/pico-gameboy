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
// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define ENABLE_SDCARD 1
#define USE_PS2_KBD 1
#define USE_NESPAD 1
#ifndef OVERCLOCKING
#define OVERCLOCKING 270
#endif

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
extern "C" {
#include "vga.h"
}

#include "f_util.h"
#include "ff.h"


#if USE_NESPAD

#include "nespad.h"

#endif

#if USE_PS2_KBD

#include "ps2kbd_mrmltr.h"

#endif

#define HOME_DIR "\\GB"
/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define FLASH_TARGET_OFFSET (1024 * 1024)
const char *rom_filename = (const char *) (XIP_BASE + FLASH_TARGET_OFFSET);
const uint8_t *rom = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET) + 4096;
static uint8_t ram[32768];

struct semaphore vga_start_semaphore;

struct gb_s gb;

enum COLORMODE {
    RGB333,
    RBG222,
};

enum INPUT {
    KEYBOARD,
    GAMEPAD1,
    GAMEPAD2,
};

typedef struct __attribute__((__packed__)) {
    uint8_t version;
    bool show_fps;
    bool flash_line;
    bool flash_frame;
    COLORMODE colormode;
    uint8_t snd_vol;
    INPUT player_1_input;
    INPUT player_2_input;
    uint8_t nes_palette;
} SETTINGS;

SETTINGS settings = {
        .version = 1,
        .show_fps = false,
        .flash_line = true,
        .flash_frame = true,
        .colormode = RBG222,
        .snd_vol = 8,
        .player_1_input = GAMEPAD1,
        .player_2_input = KEYBOARD,
        .nes_palette = 0,
};

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];

bool show_fps = false;
static FATFS fs;

#if ENABLE_SOUND
uint16_t *stream;
#endif

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )
#define RGB565_TO_RGB888(rgb565) ((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))

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

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
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

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    initVGA();
    auto *buffer = reinterpret_cast<uint8_t *>(&SCREEN[0][0]);
    setVGAbuf(buffer, LCD_WIDTH, LCD_HEIGHT);
    uint8_t *text_buf = buffer + 1000;
    setVGA_text_buf(text_buf, &text_buf[80 * 30]);
    setVGA_bg_color(0);
    setVGAbuf_pos(64, 8);
//    updatePalette(settings.palette);
    setVGA_color_flash_mode(settings.flash_line, settings.flash_frame);

    sem_acquire_blocking(&vga_start_semaphore);
}

#if ENABLE_LCD

/**
 * Draws scanline into framebuffer.
 */
void __always_inline lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t y) {
    //memcpy((uint32_t *) SCREEN[y], (uint32_t *) pixels, 160);
//         screen[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    for (unsigned int x = 0; x < LCD_WIDTH; x++)
        SCREEN[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
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

typedef struct __attribute__((__packed__)) {
    bool is_directory;
    bool is_executable;
    size_t size;
    char filename[80];
} FileItem;

int compareFileItems(const void *a, const void *b) {
    auto *itemA = (FileItem *) a;
    auto *itemB = (FileItem *) b;

    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;

    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

void __inline draw_window(char *title, int x, int y, int width, int height) {
    char textline[80];

    width--;
    height--;
    // Рисуем рамки
    // ═══
    memset(textline, 0xCD, width);
    // ╔ ╗ 188 ╝ 200 ╚
    textline[0] = 0xC9;
    textline[width] = 0xBB;
    draw_text(textline, x, y, 11, 1);
    draw_text(title, (80 - strlen(title)) >> 1, 0, 0, 3);

    textline[0] = 0xC8;
    textline[width] = 0xBC;
    draw_text(textline, x, height - y, 11, 1);


    memset(textline, ' ', width);
    textline[0] = textline[width] = 0xBA;
    for (int i = 1; i < height; i++) {
        draw_text(textline, x, i, 11, 1);
    }
}

void filebrowser_loadfile(char *pathname) {
    if (strcmp(rom_filename, pathname) == 0) {
        printf("Launching last rom");
        return;
    }

    FIL file;
    size_t bufsize = sizeof(SCREEN) & 0xfffff000;
    BYTE *buffer = (BYTE *) SCREEN;
    auto offset = FLASH_TARGET_OFFSET;
    UINT bytesRead;

    printf("Writing %s rom to flash %x\r\n", pathname, offset);
    FRESULT result = f_open(&file, pathname, FA_READ);


    if (result == FR_OK) {
        uint32_t interrupts = save_and_disable_interrupts();

        // TODO: Save it after success loading to prevent corruptions
        printf("Flashing %d bytes to flash address %x\r\n", 256, offset);
        flash_range_erase(offset, 4096);
        flash_range_program(offset, reinterpret_cast<const uint8_t *>(pathname), 256);

        offset += 4096;
        for (;;) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            result = f_read(&file, buffer, bufsize, &bytesRead);
            if (result == FR_OK) {
                if (bytesRead == 0) {
                    break;
                }

                printf("Flashing %d bytes to flash address %x\r\n", bytesRead, offset);

                printf("Erasing...");
                // Disable interupts, erase, flash and enable interrupts
                gpio_put(PICO_DEFAULT_LED_PIN, false);
                flash_range_erase(offset, bufsize);
                printf("  -> Flashing...\r\n");
                flash_range_program(offset, buffer, bufsize);

                offset += bufsize;
            } else {
                printf("Error reading rom: %d\n", result);
                break;
            }
        }

        f_close(&file);
        restore_interrupts(interrupts);
    }
}

void filebrowser(char *path, char *executable) {
    setVGAmode(VGA640x480_text_80_30);
    bool debounce = true;
    clrScr(1);
    char basepath[255];
    char tmp[80];
    strcpy(basepath, path);

    constexpr int per_page = 27;
    auto *fileItems = reinterpret_cast<FileItem *>(&SCREEN[0][0] + (1024 * 6));
    constexpr int maxfiles = (sizeof(SCREEN) - (1024 * 6)) / sizeof(FileItem);


    DIR dir;
    FILINFO fileInfo;
    FRESULT result = f_mount(&fs, "", 1);

    if (FR_OK != result) {
        printf("f_mount error: %s (%d)\r\n", FRESULT_str(result), result);
        draw_text("SD Card mount error. Halt.", 0, 1, 4, 1);
        while (1) {}
    }

    while (1) {
        int total_files = 0;
        memset(fileItems, 0, maxfiles * sizeof(FileItem));

        sprintf(tmp, " SDCARD:\\%s ", basepath);
        draw_window(tmp, 0, 0, 80, 29);

        memset(tmp, ' ', 80);
        draw_text(tmp, 0, 29, 0, 0);
        draw_text("START", 0, 29, 7, 0);
        draw_text(" Run at cursor ", 5, 29, 0, 3);

        draw_text("SELECT", 5 + 15 + 1, 29, 7, 0);
        draw_text(" Run previous  ", 5 + 15 + 1 + 6, 29, 0, 3);

        draw_text("ARROWS", 5 + 15 + 1 + 6 + 15 + 1, 29, 7, 0);
        draw_text(" Navigation    ", 5 + 15 + 1 + 6 + 15 + 1 + 6, 29, 0, 3);


        // Open the directory
        if (f_opendir(&dir, basepath) != FR_OK) {
            draw_text("Failed to open directory", 0, 1, 4, 0);
            while (1) {}
        }

        if (strlen(basepath) > 0) {
            strcpy(fileItems[total_files].filename, "..\0");
            fileItems[total_files].is_directory = true;
            fileItems[total_files].size = 0;
            total_files++;
        }

        // Read all entries from the directory
        while (f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0] != '\0' && total_files < maxfiles) {
            // Set the file item properties
            fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
            fileItems[total_files].size = fileInfo.fsize;

            // Extract the extension from the file name
            char *extension = strrchr(fileInfo.fname, '.');

            if (extension != nullptr) {
                char *token;

                token = strtok(executable, "|");
                while (token != nullptr) {
                    if (strncmp(extension + 1, token, strlen(token)) == 0) {
                        fileItems[total_files].is_executable = true;
                    }
                    token = strtok(nullptr, "|");
                }
            }

            strncpy(fileItems[total_files].filename, fileInfo.fname, 80);

            total_files++;
        }
        qsort(fileItems, total_files, sizeof(FileItem), compareFileItems);
        // Cleanup
        f_closedir(&dir);

        if (total_files > 500) {
            draw_text(" files > 500!!! ", 80 - 17, 0, 12, 3);
        }

        uint8_t color, bg_color;
        uint32_t offset = 0;
        uint32_t current_item = 0;

        while (1) {
            ps2kbd.tick();
            nespad_tick();
            sleep_ms(25);
            nespad_tick();

            if (!debounce) {
                debounce = !(!keyboard_bits.start || !gamepad_bits.start);
            }

            if (!keyboard_bits.select || !gamepad_bits.select) {
                gpio_put(PICO_DEFAULT_LED_PIN, true);
                return;
            }
            if (!keyboard_bits.down || !gamepad_bits.down) {
                if ((offset + (current_item + 1) < total_files)) {
                    if ((current_item + 1) < per_page) {
                        current_item++;
                    } else {
                        offset++;
                    }
                }
            }

            if (!keyboard_bits.up || !gamepad_bits.up) {
                if (current_item > 0) {
                    current_item--;
                } else if (offset > 0) {
                    offset--;
                }
            }

            if (!keyboard_bits.right || !gamepad_bits.right) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);

                }
            }

            if (!keyboard_bits.left || !gamepad_bits.left) {
                if (offset > per_page) {
                    offset -= per_page;
                } else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (debounce && (!keyboard_bits.start || !gamepad_bits.start)) {
                auto file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        char *lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            size_t length = lastBackslash - basepath;
                            basepath[length] = '\0';
                        }
                    } else {
                        sprintf(basepath, "%s\\%s", basepath, file_at_cursor.filename);
                    }
                    debounce = false;
                    break;
                }

                if (file_at_cursor.is_executable) {
                    sprintf(tmp, "%s\\%s", basepath, file_at_cursor.filename);
                    return filebrowser_loadfile(tmp);
                }
            }

            for (int i = 0; i < per_page; i++) {
                auto item = fileItems[offset + i];
                color = 11;
                bg_color = 1;
                if (i == current_item) {
                    color = 0;
                    bg_color = 3;

                    memset(tmp, 0xCD, 78);
                    tmp[78] = '\0';
                    draw_text(tmp, 1, per_page + 1, 11, 1);
                    sprintf(tmp, " Size: %iKb, File %lu of %i ", item.size / 1024, offset + i + 1, total_files);
                    draw_text(tmp, (80 - strlen(tmp)) >> 1, per_page + 1, 14, 3);
                }
                auto len = strlen(item.filename);
                color = item.is_directory ? 15 : color;
                color = item.is_executable ? 10 : color;
                color = strstr(rom_filename, item.filename) != nullptr ? 13 : color;

                memset(tmp, ' ', 78);
                tmp[78] = '\0';

                memcpy(&tmp, item.filename, len < 78 ? len : 78);

                draw_text(tmp, 1, i + 1, color, bg_color);
            }

            sleep_ms(100);
        }
    }
}


#endif
bool restart = false;
enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    RESET,
    RETURN,
};

typedef struct __attribute__((__packed__)) {
    const char *text;
    menu_type_e type;
    const void *value;
    uint8_t max_value;
    char value_list[5][10];
} MenuItem;
uint8_t old_resolution;
#define MENU_ITEMS_NUMBER 11

const MenuItem menu_items[MENU_ITEMS_NUMBER] = {
        //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
        //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
        { "Flash line: %s",      ARRAY, &settings.flash_line,     1, { "NO ",    "YES" }},
        { "Flash frame: %s",     ARRAY, &settings.flash_frame,    1, { "NO ",    "YES" }},
        { "VGA Mode: %s",        ARRAY, &settings.colormode,      1, { "RGB333", "RGB222" }},
        //{ "Resolution scale: %s", ARRAY, &old_resolution,          1, { "4X3", "3X3" }},
        { "Palette: %i ",        INT,   &manual_palette_selected, 12 },
        { "Show FPS: %s",        ARRAY, &show_fps,                1, { "NO ",    "YES" }},
        {},
        { "Save state",          SAVE },
        { "Load state",          LOAD },
        {},
        { "Reset to ROM select", RESET },
        { "Return to game",      RETURN }
};


void save() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    sprintf(pathname, "GB\\%s.save", filename);
    FRESULT fr = f_mount(&fs, "", 1);
    FIL fd;
    fr = f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE);
    UINT bw;

    f_write(&fd, &gb, sizeof(gb), &bw);
    f_write(&fd, ram, sizeof(ram), &bw);
    f_close(&fd);
}

void load() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    sprintf(pathname, "GB\\%s.save", filename);
    FRESULT fr = f_mount(&fs, "", 1);
    FIL fd;
    fr = f_open(&fd, pathname, FA_READ);
    UINT br;

    f_read(&fd, &gb, sizeof(gb), &br);
    f_read(&fd, ram, sizeof(ram), &br);
    f_close(&fd);
}


void load_config() {
    char pathname[255];
    sprintf(pathname, "%s\\emulator.cfg", HOME_DIR);
    FRESULT fr = f_mount(&fs, "", 1);
    FIL fd;
    fr = f_open(&fd, pathname, FA_READ);
    if (fr != FR_OK) {
        return;
    }
    UINT br;

    f_read(&fd, &settings, sizeof(settings), &br);
    f_close(&fd);
}

void save_config() {
    char pathname[255];
    sprintf(pathname, "%s\\emulator.cfg", HOME_DIR);
    FRESULT fr = f_mount(&fs, "", 1);
    FIL fd;
    fr = f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE);
    UINT bw;

    f_write(&fd, &settings, sizeof(settings), &bw);
    f_close(&fd);
}

void menu() {
    bool exit = false;
    clrScr(0);
    setVGAmode(VGA640x480_text_80_30);

    char footer[80];
    sprintf(footer, ":: %s %s build %s %s ::", PICO_PROGRAM_NAME, PICO_PROGRAM_VERSION_STRING, __DATE__, __TIME__);
    draw_text(footer, (sizeof(footer) - strlen(footer)) >> 1, 0, 11, 1);
    int current_item = 0;

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
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == NONE)
                current_item++;
        }

        if (!gamepad_bits.up) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == NONE)
                current_item--;
        }

        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + 1;
            uint8_t x = 30;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }

            const MenuItem *item = &menu_items[i];

            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto *value = (uint8_t *) item->value;

                            if (!(gamepad_bits.right) && *value < item->max_value) {
                                (*value)++;
                            }

                            if (!(gamepad_bits.left) && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case SAVE:
                        if (!gamepad_bits.start) {
                            save();
                            exit = true;
                        }
                        break;
                    case LOAD:
                        if (!gamepad_bits.start) {
                            load();
                            exit = true;
                        }
                        break;
                    case RETURN:
                        if (!gamepad_bits.start) {
                            exit = true;
                        }
                        break;
                    case RESET:
                        if (!gamepad_bits.start) {
                            restart = true;
                            exit = true;
                        }
                        break;

                }

            }
            static char result[80];

            switch (item->type) {
                case INT:
                    sprintf(result, item->text, *(uint8_t *) item->value);
                    break;
                case ARRAY:
                    sprintf(result, item->text, item->value_list[*(uint8_t *) item->value]);
                    break;
                case TEXT:
                    sprintf(result, item->text, item->value);
                    break;
                default:
                    sprintf(result, "%s", item->text);
            }

            draw_text(result, x, y, color, bg_color);
        }

        sleep_ms(100);
    }

    if (manual_palette_selected > 0) {
        manual_assign_palette(palette16, manual_palette_selected);
    }

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            palette[i][j] = i * 4 + j;
            if (settings.colormode == RGB333) {
                setVGA_color_palette(i * 4 + j, RGB565_TO_RGB888(palette16[i][j]));
            } else {
                uint32_t color = RGB565_TO_RGB888(palette16[i][j]);
                uint8_t r = (color >> (16 + 6)) & 0x3;
                uint8_t g = (color >> (8 + 6)) & 0x3;
                uint8_t b = (color >> (0 + 6)) & 0x3;

                r *= 42 * 2;
                g *= 42 * 2;
                b *= 42 * 2;
                setVGA_color_palette(i * 4 + j, RGB888(r, g, b));
            }
        }

    memset(SCREEN, 0, sizeof SCREEN);
    setVGA_color_flash_mode(settings.flash_line, settings.flash_frame);
    setVGAmode(VGA640x480div3);
    save_config();
}


int main() {
    /* Overclock. */
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);

    set_sys_clock_khz(OVERCLOCKING * 1000, true);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

#if !NDEBUG
    stdio_init_all();
        sleep_ms(3000);
#endif


    putstdio("INIT: ");

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
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

#if ENABLE_SOUND
    // Initialize audio emulation
    audio_init();

    putstdio("AUDIO ");
#endif

    load_config();
//while (1) {}
    sleep_ms(50);
    while (true) {
        manual_palette_selected = 0;
#if ENABLE_LCD
#if ENABLE_SDCARD
        /* ROM File selector */
        clrScr(0);
        setVGAmode(VGA640x480_text_80_30);
        filebrowser(HOME_DIR, "gb\0|gbc\0");

#endif
#endif
        setVGAmode(VGA640x480div3);

        /* Initialise GB context. */
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


        if (gb.cgb.cgbMode) {
//            for (uint8_t i =0; i<64; i++)
//            setVGA_color_palette(i, RGB565_TO_RGB888(gb.cgb.fixPalette[i]));
        } else {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++) {
                    palette[i][j] = i * 4 + j;
                    uint32_t color = RGB565_TO_RGB888(palette16[i][j]);
                    if (settings.colormode == RGB333) {
                        setVGA_color_palette(i * 4 + j, color);
                    } else {
                        uint8_t r = (color >> (16 + 6)) & 0x3;
                        uint8_t g = (color >> (8 + 6)) & 0x3;
                        uint8_t b = (color >> (0 + 6)) & 0x3;

                        r *= 42 * 2;
                        g *= 42 * 2;
                        b *= 42 * 2;
                        setVGA_color_palette(i * 4 + j, RGB888(r, g, b));
                    }
                }
        }
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

//gb.direct.joypad = nespad_state;
//------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            if (!gb.direct.joypad_bits.select && !gb.direct.joypad_bits.start) {
                menu();
                continue;
            }
            //-----------------------------------------------------------------
            gb_run_frame(&gb);

            //gb.direct.interlace = 1;
            frames++;

#if ENABLE_SOUND
            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
#endif
            if (show_fps && frames >= 60) {
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

        }
        restart = false;
    }

}
