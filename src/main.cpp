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


#include "audio.h"
#include "minigb_apu.h"

/* Project headers */
#include "hedley.h"
#include "peanut_gb.h"
#include "gbcolors.h"

/* Murmulator board */
#include "graphics.h"
#include "f_util.h"
#include "ff.h"


#include "nespad.h"

#include "ps2kbd_mrmltr.h"


/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define HOME_DIR (char*)"\\GB"
#define FLASH_TARGET_OFFSET (1024 * 1024)
const uint8_t* rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

static uint8_t ram[32768];

semaphore vga_start_semaphore;

gb_s gb;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
static FATFS fs;

uint16_t stream[AUDIO_BUFFER_SIZE_BYTES];

#define X2(a) (a | (a << 8))
#define X4(a) (a | (a << 8) | (a << 16) | (a << 24))
#define VGA_RGB_222(r, g, b) ((r << 4) | (g << 2) | b)
#define CHECK_BIT(var, pos) (((var)>>(pos)) & 1)

// Function to convert RGB565 to RGB222
#define convertRGB565toRGB2221(color565) \
    (((((color565 >> 11) & 0x1F) * 255 / 31) >> 6) << 4 | \
    ((((color565 >> 5) & 0x3F) * 255 / 63) >> 6) << 2 | \
    ((color565 & 0x1F) * 255 / 31) >> 6)

#if TFT
#define convertRGB565toRGB222(rgb565) (rgb565)
#else
#define convertRGB565toRGB222(rgb565) ((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))
#endif
//((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))
//((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))

typedef uint32_t palette222_t[3][4];
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

static input_bits_t keyboard_bits = { false, false, false, false, false, false, false, false }; //Keyboard
static input_bits_t gamepad_bits = { false, false, false, false, false, false, false, false }; //Joypad
//-----------------------------------------------------------------------------

void nespad_tick() {
    nespad_read();

    gamepad_bits.a = (nespad_state & DPAD_A) != 0;
    gamepad_bits.b = (nespad_state & DPAD_B) != 0;
    gamepad_bits.select = (nespad_state & DPAD_SELECT) != 0;
    gamepad_bits.start = (nespad_state & DPAD_START) != 0;
    gamepad_bits.up = (nespad_state & DPAD_UP) != 0;
    gamepad_bits.down = (nespad_state & DPAD_DOWN) != 0;
    gamepad_bits.left = (nespad_state & DPAD_LEFT) != 0;
    gamepad_bits.right = (nespad_state & DPAD_RIGHT) != 0;
}

//-----------------------------------------------------------------------------


static bool isInReport(hid_keyboard_report_t const* report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /*    printf("HID key report modifiers %2.2X report ", report->modifier);
        for (unsigned char i: report->keycode)
            printf("%2.2X", i);
        printf("\r\n");*/

    keyboard_bits.start = isInReport(report, HID_KEY_ENTER);
    keyboard_bits.select = isInReport(report, HID_KEY_BACKSPACE);
    keyboard_bits.a = isInReport(report, HID_KEY_Z);
    keyboard_bits.b = isInReport(report, HID_KEY_X);
    keyboard_bits.up = isInReport(report, HID_KEY_ARROW_UP);
    keyboard_bits.down = isInReport(report, HID_KEY_ARROW_DOWN);
    keyboard_bits.left = isInReport(report, HID_KEY_ARROW_LEFT);
    keyboard_bits.right = isInReport(report, HID_KEY_ARROW_RIGHT);
}

Ps2Kbd_Mrmltr ps2kbd(
    pio1,
    0,
    process_kbd_report);

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void __not_in_flash_func(gb_cart_ram_write)(struct gb_s* gb, const uint_fast32_t addr, const uint8_t val) {
    ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s* gb, const enum gb_error_e gb_err, const uint16_t addr) {
    const char* gb_err_str[4] = {
        "UNKNOWN",
        "INVALID OPCODE",
        "INVALID READ",
        "INVALID WRITE"
    };
    printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();
    graphics_init();

    auto* buffer = &SCREEN[0][0];
    graphics_set_buffer(buffer, LCD_WIDTH, LCD_HEIGHT);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

#if VGA
    graphics_set_offset(60, 6);
#endif
#if HDMI | TV
    graphics_set_offset(80, 0) ;
    // graphics_set_offset(80, 48) ;
#endif

    graphics_set_flashmode(false, false);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    // clrScr(1);

    sem_acquire_blocking(&vga_start_semaphore);

    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
#ifdef TFT
    uint64_t last_renderer_tick = tick;
#endif
    uint64_t last_input_tick = tick;
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            refresh_lcd();
            last_renderer_tick = tick;
        }
#endif
        if (tick >= last_input_tick + frame_tick * 1) {
            ps2kbd.tick();
            nespad_tick();
            last_input_tick = tick;
        }
        tick = time_us_64();


        // tuh_task();
        //hid_app_task();
        tight_loop_contents();
    }

    __unreachable();
}


/**
 * Draws scanline into framebuffer.
 */
void __always_inline lcd_draw_line(struct gb_s* gb, const uint8_t pixels[160], const uint_fast8_t y) {
     // memcpy((uint32_t *)SCREEN[y], (uint32_t *)pixels, 160);
    //         screen[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    for (unsigned int x = 0; x < LCD_WIDTH; x++)
    SCREEN[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
}


/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s* gb) {
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
        }
        else {
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
void write_cart_ram_file(struct gb_s* gb) {
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
        }
        else {
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
    char filename[79];
} file_item_t;

constexpr int max_files = 500;
static file_item_t fileItems[max_files];

int compareFileItems(const void* a, const void* b) {
    const auto* itemA = (file_item_t *)a;
    const auto* itemB = (file_item_t *)b;
    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;
    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

static inline bool isExecutable(const char pathname[256],const char extensions[11]) {
    char* extension = strrchr(pathname, '.');
    if (extension == nullptr) {
        return false;
    }
    extension++; // Move past the '.' character

    const char* token = strtok((char *)extensions, ","); // Tokenize the extensions string using '|'

    while (nullptr != token) {
         if (memcmp(extension, token, 3) == 0) {
            return true;
        }
        token = strtok(NULL, ",");
    }

    return false;
}

bool filebrowser_loadfile(const char pathname[256]) {
    UINT bytes_read = 0;
    FIL file;

    constexpr int window_y = (TEXTMODE_ROWS - 5) / 2;
    constexpr int window_x = (TEXTMODE_COLS - 43) / 2;

    draw_window("Loading firmware", window_x, window_y, 43, 5);

    FILINFO fileinfo;
    f_stat(pathname, &fileinfo);

    if (16384 - 64 << 10 < fileinfo.fsize) {
        draw_text("ERROR: Firmware too large! Canceled!!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(5000);
        return false;
    }


    draw_text("Loading...", window_x + 1, window_y + 2, 10, 1);
    sleep_ms(500);


    multicore_lockout_start_blocking();
    auto flash_target_offset = FLASH_TARGET_OFFSET;
    // const uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_target_offset, fileinfo.fsize);
    // restore_interrupts(ints);

    if (FR_OK == f_open(&file, pathname, FA_READ)) {
        uint8_t buffer[FLASH_PAGE_SIZE];

        do {
            f_read(&file, &buffer, FLASH_PAGE_SIZE, &bytes_read);

            if (bytes_read) {
                const uint32_t ints = save_and_disable_interrupts();
                flash_range_program(flash_target_offset, buffer, FLASH_PAGE_SIZE);
                restore_interrupts(ints);

                gpio_put(PICO_DEFAULT_LED_PIN, flash_target_offset >> 13 & 1);

                flash_target_offset += FLASH_PAGE_SIZE;
            }
        }
        while (bytes_read != 0);

        gpio_put(PICO_DEFAULT_LED_PIN, true);
    }
    f_close(&file);
    multicore_lockout_end_blocking();
    // restore_interrupts(ints);
    return true;
}

void filebrowser(const char pathname[256], const char executables[11]) {
    bool debounce = true;
    char basepath[256];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, pathname);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    if (FR_OK != f_mount(&fs, "SD", 1)) {
        draw_text("SD Card not inserted or SD Card error!", 0, 0, 12, 0);
        while (true);
    }

    memset(fileItems, 0, sizeof(file_item_t) * max_files);
    int total_files = 0;

    snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
    draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
    memset(tmp, ' ', TEXTMODE_COLS);

#ifndef TFT
    draw_text(tmp, 0, 29, 0, 0);
    auto off = 0;
    draw_text("START", off, 29, 7, 0);
    off += 5;
    draw_text(" Run at cursor ", off, 29, 0, 3);
    off += 16;
    draw_text("SELECT", off, 29, 7, 0);
    off += 6;
    draw_text(" Run previous  ", off, 29, 0, 3);
    off += 16;
    draw_text("ARROWS", off, 29, 7, 0);
    off += 6;
    draw_text(" Navigation    ", off, 29, 0, 3);
    off += 16;
    draw_text("A/F10", off, 29, 7, 0);
    off += 5;
    draw_text(" USB DRV ", off, 29, 0, 3);
#endif

    if (FR_OK != f_opendir(&dir, basepath)) {
        draw_text("Failed to open directory", 1, 1, 4, 0);
        while (true);
    }

    if (strlen(basepath) > 0) {
        strcpy(fileItems[total_files].filename, "..\0");
        fileItems[total_files].is_directory = true;
        fileItems[total_files].size = 0;
        total_files++;
    }

    while (f_readdir(&dir, &fileInfo) == FR_OK &&
           fileInfo.fname[0] != '\0' &&
           total_files < max_files
    ) {
        // Set the file item properties
        fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
        fileItems[total_files].size = fileInfo.fsize;
        fileItems[total_files].is_executable = isExecutable(fileInfo.fname, executables);
        strncpy(fileItems[total_files].filename, fileInfo.fname, 78);
        total_files++;
    }
    f_closedir(&dir);

    qsort(fileItems, total_files, sizeof(file_item_t), compareFileItems);

    if (total_files > max_files) {
        draw_text(" Too many files!! ", TEXTMODE_COLS - 17, 0, 12, 3);
    }

    int offset = 0;
    int current_item = 0;

    while (true) {
        sleep_ms(100);

        if (!debounce) {
            debounce = !(nespad_state & DPAD_START) && !keyboard_bits.start;
        }

        // ESCAPE
        if (nespad_state & DPAD_SELECT || keyboard_bits.select) {
            return;
        }

        if (nespad_state & DPAD_DOWN || keyboard_bits.down) {
            if (offset + (current_item + 1) < total_files) {
                if (current_item + 1 < per_page) {
                    current_item++;
                }
                else {
                    offset++;
                }
            }
        }

        if (nespad_state & DPAD_UP || keyboard_bits.up) {
            if (current_item > 0) {
                current_item--;
            }
            else if (offset > 0) {
                offset--;
            }
        }

        if (nespad_state & DPAD_RIGHT || keyboard_bits.right) {
            offset += per_page;
            if (offset + (current_item + 1) > total_files) {
                offset = total_files - (current_item + 1);
            }
        }

        if (nespad_state & DPAD_LEFT || keyboard_bits.left) {
            if (offset > per_page) {
                offset -= per_page;
            }
            else {
                offset = 0;
                current_item = 0;
            }
        }

        if (debounce && (nespad_state & DPAD_START || keyboard_bits.start)) {
            auto file_at_cursor = fileItems[offset + current_item];

            if (file_at_cursor.is_directory) {
                if (strcmp(file_at_cursor.filename, "..") == 0) {
                    const char* lastBackslash = strrchr(basepath, '\\');
                    if (lastBackslash != nullptr) {
                        const size_t length = lastBackslash - basepath;
                        basepath[length] = '\0';
                    }
                }
                else {
                    sprintf(basepath, "%s\\%s", basepath, file_at_cursor.filename);
                }
                debounce = false;
                break;
            }

            if (file_at_cursor.is_executable) {
                sprintf(tmp, "%s\\%s", basepath, file_at_cursor.filename);

                filebrowser_loadfile(tmp);
                return;
            }
        }

        for (int i = 0; i < per_page; i++) {
            uint8_t color = 11;
            uint8_t bg_color = 1;

            if (offset+i < max_files) {
                const auto item = fileItems[offset + i];


                if (i == current_item) {
                    color = 0;
                    bg_color = 3;
                    memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                    tmp[TEXTMODE_COLS - 2] = '\0';
                    draw_text(tmp, 1, per_page + 1, 11, 1);
                    snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024, offset + i + 1,
                             total_files);
                    draw_text(tmp, 2, per_page + 1, 14, 3);
                }

                const auto len = strlen(item.filename);
                color = item.is_directory ? 15 : color;
                color = item.is_executable ? 10 : color;
                //color = strstr((char *)rom_filename, item.filename) != nullptr ? 13 : color;

                memset(tmp, ' ', TEXTMODE_COLS - 2);
                tmp[TEXTMODE_COLS - 2] = '\0';
                memcpy(&tmp, item.filename, len < TEXTMODE_COLS - 2 ? len : TEXTMODE_COLS - 2);
            } else {
                memset(tmp, ' ', TEXTMODE_COLS - 2);
            }
            draw_text(tmp, 1, i + 1, color, bg_color);
        }
    }
}


bool restart = false;

enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    ROM_SELECT,
    RETURN,
};

typedef struct __attribute__((__packed__)) {
    const char* text;
    menu_type_e type;
    const void* value;
    uint8_t max_value;
    char value_list[5][10];
} MenuItem;

const MenuItem menu_items[] = {
    //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    { "Palette: %i ", INT, &manual_palette_selected, 12 },
    {},
    { "Save state", SAVE },
    { "Load state", LOAD },
    {},
    { "Reset to ROM select", ROM_SELECT },
    { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

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

void menu() {
    bool exit = false;
    graphics_set_mode(TEXTMODE_DEFAULT);
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
    uint current_item = 0;

    while (!exit) {
        sleep_ms(25);
        if (gamepad_bits.down || keyboard_bits.down) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == NONE)
                current_item++;
        }
        if (gamepad_bits.up || keyboard_bits.up) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == NONE)
                current_item--;
        }
        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + (TEXTMODE_ROWS - MENU_ITEMS_NUMBER >> 1);
            uint8_t x = TEXTMODE_COLS / 2 - 10;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            const MenuItem* item = &menu_items[i];
            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto* value = (uint8_t *)item->value;
                            if ((gamepad_bits.right || keyboard_bits.right) && *value < item->max_value) {
                                (*value)++;
                            }
                            if ((gamepad_bits.left || keyboard_bits.left) && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case SAVE:
                        if (gamepad_bits.start || keyboard_bits.start) {
                            save();
                            exit = true;
                        }
                        break;
                    case LOAD:
                        if (gamepad_bits.start || keyboard_bits.start) {
                            load();
                            exit = true;
                        }
                        break;
                    case RETURN:
                        if (gamepad_bits.start || keyboard_bits.start)
                            exit = true;
                        break;

                    case ROM_SELECT:
                        if (gamepad_bits.start || keyboard_bits.start) {
                            restart = true;
                            return;
                        }
                        break;
                }
            }
            static char result[TEXTMODE_COLS];
            switch (item->type) {
                case INT:
                    snprintf(result, TEXTMODE_COLS, item->text, *(uint8_t *)item->value);
                    break;
                case ARRAY:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *)item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
        }
        sleep_ms(100);
    }
    if (manual_palette_selected > 0) {
        manual_assign_palette(palette16, manual_palette_selected);

        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                graphics_set_palette(i * 4 + j, convertRGB565toRGB222(palette16[i][j]));
                palette[i][j] = i * 4 + j;
            }

    }

    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}


int main() {
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    set_sys_clock_khz(378 * KHZ, true);

    memset(&SCREEN[0][0], 0, sizeof SCREEN);

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sleep_ms(50);

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }


    // Initialize I2S sound driver
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);

    // Initialize audio emulation
    audio_init();

    while (true) {
        manual_palette_selected = 0;
        /* ROM File selector */

        graphics_set_mode(TEXTMODE_DEFAULT);
        filebrowser(HOME_DIR, "gb\0,gbc");
        graphics_set_mode(GRAPHICSMODE_DEFAULT);

        /* Initialise GB context. */
        gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                      &gb_cart_ram_write, &gb_error, nullptr);

        if (ret != GB_INIT_NO_ERROR) {
            while (1) draw_text("error", 1, 1, 1, 2);
            sleep_ms(100);
        }

        /* Automatically assign a colour palette to the game */
        if (!manual_palette_selected) {
            char rom_title[16];
            auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
        }
        else {
            manual_assign_palette(palette16, manual_palette_selected);
        }

        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                graphics_set_palette(i * 4 + j, convertRGB565toRGB222(palette16[i][j]));
                palette[i][j] = i * 4 + j;
            }
                //palette[i][j] = convertRGB565toRGB222(palette16[i][j]);

        gb_init_lcd(&gb, &lcd_draw_line);
        /* Load Save File. */
        read_cart_ram_file(&gb);

        //=============================================================================
        while (!restart) {
            //------------------------------------------------------------------------------

            gb.direct.joypad_bits.up = !keyboard_bits.up && !gamepad_bits.up;
            gb.direct.joypad_bits.down = !keyboard_bits.down && !gamepad_bits.down;
            gb.direct.joypad_bits.left = !keyboard_bits.left && !gamepad_bits.left;
            gb.direct.joypad_bits.right = !keyboard_bits.right && !gamepad_bits.right;
            gb.direct.joypad_bits.a = !keyboard_bits.a && !gamepad_bits.a;
            gb.direct.joypad_bits.b = !keyboard_bits.b && !gamepad_bits.b;
            gb.direct.joypad_bits.select = !keyboard_bits.select && !gamepad_bits.select;
            gb.direct.joypad_bits.start = !keyboard_bits.start && !gamepad_bits.start;

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

            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
            gpio_put(PICO_DEFAULT_LED_PIN, false);
        }
        restart = false;
    }
}
