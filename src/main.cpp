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


/* C Headers */
#include <cstdio>
#include <cstring>

/* RP2040 Headers */
#include "pico/runtime.h"
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#if PICO_RP2350
#include <hardware/structs/qmi.h>
#endif
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>
#include <hardware/watchdog.h>
#include <hardware/clocks.h>

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
#include "psram.h"


/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define HOME_DIR (char*)"\\GB"
extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (((((uintptr_t)&__flash_binary_end - XIP_BASE) / FLASH_SECTOR_SIZE) + 4) * FLASH_SECTOR_SIZE)
#define FLASH_ROM ((const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET))
/* the cartridge being played: in flash, or in QSPI PSRAM when there is one */
static const uint8_t* rom = FLASH_ROM;

/* A ROM loaded into PSRAM survives a watchdog reset (SELECT in the browser
 * runs it again: PSRAM keeps its contents while powered).  After power-up the
 * uninitialized RAM is random, hence the magic; then the flash copy is used. */
#define ROM_INFO_MAGIC 0x31524F47u
static uint32_t __uninitialized_ram(rom_magic);
static uint32_t __uninitialized_ram(rom_in_psram);
static uint32_t __uninitialized_ram(rom_psram_size);

/* Flash is an external chip, its size does not depend on the board header.
 * Read it from JEDEC ID (0x9F) like murmulator_os2: the third ID byte is
 * log2(capacity).  Called first thing in main(), before overclocking. */
static size_t flash_bytes = PICO_FLASH_SIZE_BYTES;

static void __no_inline_not_in_flash_func(detect_flash_size)() {
    uint8_t tx[4] = { 0x9f, 0, 0, 0 };
    uint8_t rx[4] = { 0 };
    const uint32_t ints = save_and_disable_interrupts();
#if PICO_RP2350
    /* the SDK re-enters XIP with the boot-time QMI window-0 setup */
    const uint32_t timing = qmi_hw->m[0].timing;
    const uint32_t rfmt = qmi_hw->m[0].rfmt;
    const uint32_t rcmd = qmi_hw->m[0].rcmd;
#endif
    flash_do_cmd(tx, rx, sizeof tx);
#if PICO_RP2350
    qmi_hw->m[0].timing = timing;
    qmi_hw->m[0].rfmt = rfmt;
    qmi_hw->m[0].rcmd = rcmd;
    __asm volatile ("dsb; isb" ::: "memory");
#endif
    restore_interrupts(ints);
    /* 16 MiB is the XIP window of one chip select */
    if (rx[3] >= 20 && rx[3] <= 24)
        flash_bytes = (size_t)1 << rx[3];
}

/* The ROM goes right after the firmware; the top 512 KB of the flash stay
 * untouched (pico-launcher lives in the last 260 KB). */
static size_t flash_rom_capacity() {
    const size_t end = flash_bytes - (512u << 10);
    return end > FLASH_TARGET_OFFSET ? end - FLASH_TARGET_OFFSET : 0;
}

static bool rom_fits(const size_t size) {
    return size != 0 && (size <= flash_rom_capacity() ||
                         (gb_psram_available() && size <= gb_psram_size()));
}

/* the cartridge "Run previous" (SELECT) starts */
static const uint8_t* previous_rom() {
    if (rom_magic == ROM_INFO_MAGIC && rom_in_psram &&
        gb_psram_available() && rom_psram_size <= gb_psram_size())
        return (const uint8_t *)GB_PSRAM_BASE;
    return FLASH_ROM;
}

/* All watchdog resets (Ctrl+Alt+Del) take the same route: release QMI CS1 so
 * the PSRAM does not stay in QPI mode across reset, then drop clk_sys before
 * VREG, so the boot ROM and the next firmware start at a safe clock. */
extern "C" void __no_inline_not_in_flash_func(gb_safe_reset)(void) {
    watchdog_enable(100, true); // failsafe in case QMI/clock transition stalls
    save_and_disable_interrupts();
#if PICO_RP2350
    gb_psram_prepare_reset();
    set_sys_clock_khz(150000, false);
    vreg_set_voltage(VREG_VOLTAGE_1_10);
    sleep_ms(10);
#else
    set_sys_clock_khz(125000, false);
#endif
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

static uint8_t ram[32768];

semaphore vga_start_semaphore;
// set by the render core once text/graphics buffers are attached to the video driver
static volatile bool render_ready = false;

gb_s gb;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
static FATFS fs;

uint16_t stream[AUDIO_BUFFER_SIZE_BYTES];

#if TFT
#define RGB565_TO_RGB888(rgb565) (rgb565)
#else
#define RGB565_TO_RGB888(rgb565) ((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))
#endif

typedef uint32_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette
static uint8_t manual_palette_selected = 0; // auto

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

static uint8_t swap_ab = 0;
static input_bits_t keyboard = { false, false, false, false, false, false, false, false }; //Keyboard
static input_bits_t gamepad_bits = { false, false, false, false, false, false, false, false }; //Joypad
//-----------------------------------------------------------------------------

void nespad_tick() {
    nespad_read();

    if (swap_ab) {
        gamepad_bits.b = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.a = keyboard.b || (nespad_state & DPAD_B) != 0;
    } else {
        gamepad_bits.a = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.b = keyboard.b || (nespad_state & DPAD_B) != 0;
    }
    gamepad_bits.select = keyboard.select || (nespad_state & DPAD_SELECT) != 0;
    gamepad_bits.start = keyboard.start || (nespad_state & DPAD_START) != 0;
    gamepad_bits.up = keyboard.up || (nespad_state & DPAD_UP) != 0;
    gamepad_bits.down = keyboard.down || (nespad_state & DPAD_DOWN) != 0;
    gamepad_bits.left = keyboard.left || (nespad_state & DPAD_LEFT) != 0;
    gamepad_bits.right = keyboard.right || (nespad_state & DPAD_RIGHT) != 0;
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

static volatile bool altPressed = false;
static volatile bool ctrlPressed = false;
static volatile uint8_t fxPressedV = 0;
static volatile bool kbd_page_up = false;
static volatile bool kbd_page_down = false;
static volatile bool kbd_home = false;
static volatile bool kbd_end = false;

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /*    printf("HID key report modifiers %2.2X report ", report->modifier);
        for (unsigned char i: report->keycode)
            printf("%2.2X", i);
        printf("\r\n");*/
    keyboard.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    keyboard.a = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_KEYPAD_0);
    keyboard.b = isInReport(report, HID_KEY_X) || isInReport(report, HID_KEY_P) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);

    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    keyboard.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || isInReport(report, HID_KEY_W) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || isInReport(report, HID_KEY_S) || isInReport(report, HID_KEY_KEYPAD_2) || isInReport(report, HID_KEY_KEYPAD_5);
    keyboard.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || isInReport(report, HID_KEY_A) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || isInReport(report, HID_KEY_D) || isInReport(report, HID_KEY_KEYPAD_6);

    kbd_page_up = isInReport(report, HID_KEY_PAGE_UP);
    kbd_page_down = isInReport(report, HID_KEY_PAGE_DOWN);
    kbd_home = isInReport(report, HID_KEY_HOME);
    kbd_end = isInReport(report, HID_KEY_END);

    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);

    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        gb_safe_reset();
    }
    if (ctrlPressed || altPressed) {
        uint8_t fxPressed = 0;
        if (isInReport(report, HID_KEY_F1)) fxPressed = 1;
        else if (isInReport(report, HID_KEY_F2)) fxPressed = 2;
        else if (isInReport(report, HID_KEY_F3)) fxPressed = 3;
        else if (isInReport(report, HID_KEY_F4)) fxPressed = 4;
        else if (isInReport(report, HID_KEY_F5)) fxPressed = 5;
        else if (isInReport(report, HID_KEY_F6)) fxPressed = 6;
        else if (isInReport(report, HID_KEY_F7)) fxPressed = 7;
        else if (isInReport(report, HID_KEY_F8)) fxPressed = 8;
        fxPressedV = fxPressed;
    }
}

Ps2Kbd_Mrmltr ps2kbd(
    pio1,
    PS2KBD_GPIO_FIRST,
    process_kbd_report
);

enum : uint8_t {
    BTN_UP     = 1u << 0,
    BTN_DOWN   = 1u << 1,
    BTN_LEFT   = 1u << 2,
    BTN_RIGHT  = 1u << 3,
    BTN_A      = 1u << 4,
    BTN_B      = 1u << 5,
    BTN_START  = 1u << 6,
    BTN_SELECT = 1u << 7,
};

/* keyboard and NES pad together (nespad_tick() merges them) */
static uint8_t input_held() {
    uint8_t b = 0;
    if (gamepad_bits.up) b |= BTN_UP;
    if (gamepad_bits.down) b |= BTN_DOWN;
    if (gamepad_bits.left) b |= BTN_LEFT;
    if (gamepad_bits.right) b |= BTN_RIGHT;
    if (gamepad_bits.a) b |= BTN_A;
    if (gamepad_bits.b) b |= BTN_B;
    if (gamepad_bits.start) b |= BTN_START;
    if (gamepad_bits.select) b |= BTN_SELECT;
    return b;
}

/* Buttons that are already down when the mode changes (the Enter that started
 * a ROM, the Start+Select that opened the menu, Enter on "Return to game",
 * Enter on a directory...) are ignored until they are released. */
struct input_gate_t {
    uint8_t mask = 0;

    void arm() { mask = input_held(); }

    uint8_t read() {
        const uint8_t held = input_held();
        mask &= held;
        return held & (uint8_t)~mask;
    }
};

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
#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
#endif
    graphics_init();

    const auto buffer = (uint8_t *)SCREEN;
    graphics_set_buffer(buffer, LCD_WIDTH, LCD_HEIGHT);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

#if VGA
    graphics_set_offset(60, 6);
#endif
#if HDMI | TV
    graphics_set_offset(80, 48);
#endif
#if SOFTTV
    graphics_set_offset(0, 48); // the TV driver centres the picture itself, with square pixels
#endif

    graphics_set_flashmode(false, false);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    // clrScr(1);

    render_ready = true;
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


#ifdef KBDUSB
        tuh_task();
#endif
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
    if (gb->cgb.cgbMode) {
        memcpy((uint32_t *)SCREEN[y], (uint32_t *)pixels, 160);
    }
    else {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
            SCREEN[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    }
}

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s* gb) {
    char filename[24];
    uint_fast32_t save_size;
    UINT br;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_READ);
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
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
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

bool isExecutable(const char pathname[255],const char *extensions) {
    char *pathCopy = strdup(pathname);
    const char* token = strrchr(pathCopy, '.');

    if (token == nullptr) {
        return false;
    }

    token++;

    while (token != NULL) {
        if (strstr(extensions, token) != NULL) {
            free(pathCopy);
            return true;
        }
        token = strtok(NULL, ",");
    }
    free(pathCopy);
    return false;
}

#if PICO_RP2350
/* flash_range_erase/program leave XIP through the boot-time XIP setup, which
 * restores the QMI window-0 timing chosen for the boot clock.  overclock()
 * runs the flash with its own timing at 378+ MHz, so the boot value would
 * clock the flash far too fast afterwards and every XIP fetch would return
 * garbage.  Keep the overclock setup; nothing runs from flash in between. */
static void __no_inline_not_in_flash_func(flash_write_sector)(const uint32_t offset, const uint8_t* data) {
    const uint32_t qmi_timing = qmi_hw->m[0].timing;
    const uint32_t qmi_rfmt = qmi_hw->m[0].rfmt;
    const uint32_t qmi_rcmd = qmi_hw->m[0].rcmd;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    qmi_hw->m[0].timing = qmi_timing;
    qmi_hw->m[0].rfmt = qmi_rfmt;
    qmi_hw->m[0].rcmd = qmi_rcmd;
    __asm volatile ("dsb; isb" ::: "memory");
}
#else
static void __no_inline_not_in_flash_func(flash_write_sector)(const uint32_t offset, const uint8_t* data) {
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
}
#endif

/* QSPI PSRAM, when found, takes the ROM directly (no flash wear, no erase);
 * otherwise it goes to flash sector by sector: a sector that already holds
 * the same data is not erased again, and every written sector is verified. */
bool filebrowser_loadfile(const char pathname[256]) {
    FIL file;
    FILINFO fileinfo;

    const int window_w = (int)TEXTMODE_COLS < 43 ? (int)TEXTMODE_COLS : 43; // TV NTSC has only 40 columns
    const int window_y = (TEXTMODE_ROWS - 5) / 2;
    const int window_x = ((int)TEXTMODE_COLS - window_w) / 2;

    auto fail = [&](const char* message) {
        draw_text(message, window_x + 1, window_y + 2, 13, 1);
        sleep_ms(3000);
        return false;
    };

    draw_window("Loading ROM", window_x, window_y, window_w, 5);

    if (FR_OK != f_stat(pathname, &fileinfo) || fileinfo.fsize == 0)
        return fail("ERROR: Can't read the ROM file!   ");

    const size_t load_size = fileinfo.fsize;
    const bool use_psram = gb_psram_available() && load_size <= gb_psram_size();
    if (!use_psram && load_size > flash_rom_capacity())
        return fail("ERROR: ROM too large! Canceled!!  ");

    if (FR_OK != f_open(&file, pathname, FA_READ))
        return fail("ERROR: Can't open the ROM file!   ");

    draw_text(use_psram ? "Loading into QSPI PSRAM..." : "Loading into Flash...",
              window_x + 1, window_y + 2, 10, 1);

    size_t total_read = 0;
    UINT bytes_read = 0;
    bool verify_failed = false;

    if (use_psram) {
        rom_magic = 0; // the PSRAM copy is being overwritten
        uint8_t* dst = (uint8_t *)GB_PSRAM_BASE;
        while (total_read < load_size) {
            const UINT chunk = load_size - total_read > 4096 ? 4096 : (UINT)(load_size - total_read);
            if (FR_OK != f_read(&file, dst + total_read, chunk, &bytes_read) || bytes_read != chunk)
                break;
            total_read += bytes_read;
        }
    }
    else {
        /* the sector buffer lives in the tail of SCREEN, behind the text
         * mode: the game redraws the whole picture anyway */
        uint8_t* const buffer = &SCREEN[0][0] + sizeof(SCREEN) - FLASH_SECTOR_SIZE;
#ifdef TEXTMODE_COLS_MAX
        static_assert(TEXTMODE_COLS_MAX * TEXTMODE_ROWS * 2 + FLASH_SECTOR_SIZE <= sizeof(SCREEN),
                      "no room for the flash sector buffer");
#else
        static_assert(TEXTMODE_COLS * TEXTMODE_ROWS * 2 + FLASH_SECTOR_SIZE <= sizeof(SCREEN),
                      "no room for the flash sector buffer");
#endif
        static_assert(sizeof(SCREEN) % 4 == 0, "flash sector buffer must stay word aligned");
        uint32_t target = FLASH_TARGET_OFFSET;
        while (total_read < load_size) {
            const UINT chunk = load_size - total_read > FLASH_SECTOR_SIZE
                                   ? FLASH_SECTOR_SIZE : (UINT)(load_size - total_read);
            memset(buffer, 0xff, FLASH_SECTOR_SIZE);
            if (FR_OK != f_read(&file, buffer, chunk, &bytes_read) || bytes_read != chunk)
                break;
            total_read += bytes_read;

            const auto* mapped = (const uint8_t *)(XIP_BASE + target);
            if (memcmp(mapped, buffer, FLASH_SECTOR_SIZE) != 0) {
                multicore_lockout_start_blocking();
                const uint32_t ints = save_and_disable_interrupts();
                flash_write_sector(target, buffer);
                restore_interrupts(ints);
                multicore_lockout_end_blocking();
                if (memcmp(mapped, buffer, FLASH_SECTOR_SIZE) != 0) {
                    verify_failed = true;
                    break;
                }
            }
            target += FLASH_SECTOR_SIZE;
            gpio_put(PICO_DEFAULT_LED_PIN, target >> 13 & 1);
        }
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        memset(buffer, 0, FLASH_SECTOR_SIZE);
    }
    f_close(&file);

    if (verify_failed)
        return fail("ERROR: Flash verify failed!       ");
    if (total_read != load_size)
        return fail("ERROR: ROM load failed!           ");

    if (use_psram) {
        rom_psram_size = load_size;
        rom_in_psram = 1;
        rom_magic = ROM_INFO_MAGIC;
        rom = (const uint8_t *)GB_PSRAM_BASE;
    }
    else {
        rom_in_psram = 0; // a PSRAM copy, if any, is no longer the latest ROM
        rom = FLASH_ROM;
    }
    return true;
}

/* Where the browser was left: re-entering it opens the same directory with
 * the same entry under the cursor (e.g. the ROM that was just played). */
static char fb_dir[256] = "\\GB";
static char fb_focus[80] = "";

static void fb_remember(const char* dir, const char* name) {
    strncpy(fb_dir, dir, sizeof fb_dir - 1);
    fb_dir[sizeof fb_dir - 1] = '\0';
    strncpy(fb_focus, name, sizeof fb_focus - 1);
    fb_focus[sizeof fb_focus - 1] = '\0';
}

/* Returns when a ROM is ready to run: a newly loaded one (START) or the
 * previous one (SELECT). */
void filebrowser(const char executables[11]) {
    input_gate_t gate;
    char basepath[256];
    char focus[80];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, fb_dir);
    strcpy(focus, fb_focus);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    while (true) {
        gate.arm();
        memset(fileItems, 0, sizeof(file_item_t) * max_files);
        int total_files = 0;

        if (FR_OK != f_opendir(&dir, basepath)) {
            if (strcmp(basepath, HOME_DIR) != 0) {
                // the remembered directory is gone (another SD card?)
                strcpy(basepath, HOME_DIR);
                focus[0] = '\0';
                continue;
            }
            draw_text("Failed to open directory", 1, 1, 4, 0);
            while (true);
        }

        snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
        draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
        draw_text(gb_psram_available() ? "QSPI PSRAM" : "FLASH", TEXTMODE_COLS - 11, 0, 10, 1);
        memset(tmp, ' ', TEXTMODE_COLS);


        draw_text(tmp, 0, 29, 0, 0);
        auto off = 0;
        draw_text("START", off, 29, 7, 0);
        off += 5;
        draw_text(" Run at cursor ", off, 29, 0, 3);
        off += 16;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Run previous  ", off, 29, 0, 3);
#ifndef TFT
        off += 16;
        draw_text("ARROWS", off, 29, 7, 0);
        off += 6;
        draw_text(" Navigation    ", off, 29, 0, 3);
        off += 16;
        draw_text("A/F10", off, 29, 7, 0);
        off += 5;
        draw_text(" USB DRV ", off, 29, 0, 3);
#endif

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
            fileItems[total_files].is_executable = !(fileInfo.fattrib & AM_DIR) &&
                                                   rom_fits(fileInfo.fsize) &&
                                                   isExecutable(fileInfo.fname, executables);
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

        /* Scroll the remembered entry into view, near the middle of the page
         * when possible and clamped at the beginning/end of the list. */
        if (focus[0]) {
            for (int i = 0; i < total_files; ++i) {
                if (!strcmp(fileItems[i].filename, focus)) {
                    constexpr int preferred_row = per_page / 2;
                    const int max_offset = total_files > per_page ? total_files - per_page : 0;
                    offset = i > preferred_row ? i - preferred_row : 0;
                    if (offset > max_offset) offset = max_offset;
                    current_item = i - offset;
                    break;
                }
            }
            focus[0] = '\0';
        }

        while (true) {
            sleep_ms(100);

            const uint8_t keys = gate.read();

            // SELECT: run the previous cartridge
            if (keys & BTN_SELECT) {
                fb_remember(basepath, fileItems[offset + current_item].filename);
                rom = previous_rom();
                return;
            }

            if (keys & BTN_DOWN) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    }
                    else {
                        offset++;
                    }
                }
            }

            if (keys & BTN_UP) {
                if (current_item > 0) {
                    current_item--;
                }
                else if (offset > 0) {
                    offset--;
                }
            }

            if (keys & BTN_RIGHT) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (keys & BTN_LEFT) {
                if (offset > per_page) {
                    offset -= per_page;
                }
                else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (total_files > 0 && (kbd_page_down || kbd_page_up || kbd_home || kbd_end)) {
                const int last = total_files - 1;
                const int per_page_half = per_page >> 1;
                const int max_offset = total_files > per_page_half ? total_files - per_page_half : 0;
                int pos = offset + current_item;
                if (kbd_page_down) {
                    pos += per_page_half;
                    offset += per_page_half;
                }
                else if (kbd_page_up) {
                    pos -= per_page_half;
                    offset -= per_page_half;
                }
                else if (kbd_home) {
                    pos = 0;
                    offset = 0;
                }
                else {
                    pos = last;
                    offset = max_offset;
                }
                if (pos > last) pos = last;
                if (pos < 0) pos = 0;
                if (offset > max_offset) offset = max_offset;
                if (offset < 0) offset = 0;
                if (pos < offset) offset = pos;
                if (pos >= offset + per_page_half) offset = pos - per_page_half + 1;
                current_item = pos - offset;
            }

            if (keys & BTN_START) {
                const auto& file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        const char* lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            // come back to the directory we have just left
                            strncpy(focus, lastBackslash + 1, sizeof focus - 1);
                            focus[sizeof focus - 1] = '\0';
                            basepath[lastBackslash - basepath] = '\0';
                        }
                    }
                    else {
                        char subdir[256];
                        if (snprintf(subdir, sizeof subdir, "%s\\%s", basepath, file_at_cursor.filename) <
                            (int)sizeof subdir)
                            strcpy(basepath, subdir);
                    }
                    break;
                }

                if (file_at_cursor.is_executable) {
                    char fullpath[256];
                    if (snprintf(fullpath, sizeof fullpath, "%s\\%s", basepath, file_at_cursor.filename) <
                        (int)sizeof fullpath && filebrowser_loadfile(fullpath)) {
                        fb_remember(basepath, file_at_cursor.filename);
                        return;
                    }
                    // the error window covered the list: redraw it, same entry
                    strcpy(focus, file_at_cursor.filename);
                    break;
                }
            }

            for (int i = 0; i < per_page; i++) {
                uint8_t color = 11;
                uint8_t bg_color = 1;

                if (offset + i < max_files) {
                    const auto item = fileItems[offset + i];


                    if (i == current_item) {
                        color = 0;
                        bg_color = 3;
                        memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                        tmp[TEXTMODE_COLS - 2] = '\0';
                        draw_text(tmp, 1, per_page + 1, 11, 1);
                        snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024,
                                 offset + i + 1,
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
                }
                else {
                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                }
                draw_text(tmp, 1, i + 1, color, bg_color);
            }
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

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char* text;
    menu_type_e type;
    const void* value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[15][15];
} MenuItem;

static int save_slot = 0;
#ifndef CPU_FREQ
#define CPU_FREQ 378
#endif
#define FREQ_STR_(x) #x
#define FREQ_STR(x) FREQ_STR_(x)
static uint16_t frequencies[] = { CPU_FREQ, 396, 404, 408, 412, 416, 420, 424, 433 };
static uint8_t frequency_index = 0;

bool overclock() {
#if PICO_RP2350
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(frequencies[frequency_index] * KHZ, false);
    *qmi_m0_timing = 0x60007303;
    gb_psram_reclock(); // QMI CS1 timing depends on clk_sys (no-op without PSRAM)
    return true;
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);
    return set_sys_clock_khz(frequencies[frequency_index] * KHZ, true);
#endif
}

static bool save() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    if (save_slot) {
        sprintf(pathname, "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    }
    else {
        sprintf(pathname, "%s\\%s.save", HOME_DIR, filename);
    }

    FIL fd;
    f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE);
    UINT bw;
    f_write(&fd, &gb, sizeof(gb), &bw);
    f_write(&fd, ram, sizeof(ram), &bw);
    f_close(&fd);

    return true;
}

static bool load() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    if (save_slot) {
        sprintf(pathname, "GB\\%s_%d.save", filename, save_slot);
    }
    else {
        sprintf(pathname, "GB\\%s.save", filename);
    }

    FIL fd;
    f_open(&fd, pathname, FA_READ);
    UINT br;
    f_read(&fd, &gb, sizeof(gb), &br);
    f_read(&fd, ram, sizeof(ram), &br);
    f_close(&fd);
    return true;
}
/* TV-SOFT: a TV standard is a coherent set (lines, colour subcarrier,
 * sync), so the menu only selects the system; the game is always in colour. */
static uint8_t tv_system = 0; // 0 - PAL, 1 - NTSC
/* HDMI / VGA: picture size on 640x480 (enum graphics_scale_t), by default as
 * the drivers showed it before: HDMI 2x, VGA 3x */
#if VGA
static uint8_t screen_scale = 1;
#else
static uint8_t screen_scale = 0;
#endif
#ifdef TEXTMODE_COLS_MAX // text columns change at run time (TV-SOFT)
#define TEXT_BUFFER_COLS TEXTMODE_COLS_MAX
#else
#define TEXT_BUFFER_COLS TEXTMODE_COLS
#endif
const MenuItem menu_items[] = {
    //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    { "Swap AB <> BA: %s", ARRAY, &swap_ab,  nullptr, 1, {"NO ", "YES"}},
    { "Palette: %s ", ARRAY, &manual_palette_selected, nullptr, 12,
        {
            "0 - AUTO      ",
            "1 - yellow-red",
            "2 - orange    ",
            "3 - negative  ",
            "4 - dark green",
            "5 - red       ",
            "6 - pink      ",
            "7 - green     ",
            "8 - dark blue ",
            "9 - pastel    ",
            "10 - blue     ",
            "11 - yellow   ",
            "12 - DMG      "
        }
    },
    {},
    { "Save state: %i", INT, &save_slot, &save, 8 },
    { "Load state: %i", INT, &save_slot, &load, 8 },
#if SOFTTV
    { "" },
    { "TV system %s", ARRAY, &tv_system, nullptr, 1, { "PAL ", "NTSC" } },
#endif
#if VGA || HDMI
    { "" },
    { "Screen: %s", ARRAY, &screen_scale, nullptr, 2, { "2x  ", "3x  ", "Full" } },
#endif
    {},
{
    "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
    { FREQ_STR(CPU_FREQ), "396", "404", "408", "412", "416", "420", "424", "432" }
},
{ "Press START / Enter to apply", NONE },
    { "Reset to ROM select", ROM_SELECT },
    { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

static void f_load_conf(void) {
    FIL f;
    if (f_open(&f, "/GB/gb.conf", FA_READ) == FR_OK) {
        UINT br;
        f_read(&f, &swap_ab, 1, &br);
        f_read(&f, &manual_palette_selected, 1, &br);
        // the same layout for all builds; older configs end earlier
        uint8_t v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= 1) tv_system = v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= GRAPHICS_SCALE_STRETCH) screen_scale = v;
        f_close(&f);
    }
}

static void f_save_conf(void) {
    f_mkdir("/GB"); // ничего не делает, если она уже есть
    FIL f;
    f_open(&f, "/GB/gb.conf", FA_CREATE_ALWAYS | FA_WRITE);
    UINT br;
    f_write(&f, &swap_ab, 1, &br);
    f_write(&f, &manual_palette_selected, 1, &br);
    f_write(&f, &tv_system, 1, &br);
    f_write(&f, &screen_scale, 1, &br);
    f_close(&f);
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
    /* Only fresh presses count: the Start+Select that opened the menu must be
     * released first, so it can't change or activate the first item. */
    uint8_t prev_keys = input_held();

    while (!exit) {
        const uint8_t held_keys = input_held();
        const uint8_t pressed = held_keys & (uint8_t)~prev_keys;
        prev_keys = held_keys;
        const bool activate = pressed & BTN_START;

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
                            if ((pressed & BTN_RIGHT) && *value < item->max_value) {
                                (*value)++;
                            }
                            if ((pressed & BTN_LEFT) && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case RETURN:
                        if (activate)
                            exit = true;
                        break;

                    case ROM_SELECT:
                        if (activate) {
                            restart = true;
                            return;
                        }
                        break;
                    default:
                        break;
                }

                if (nullptr != item->callback && activate) {
                    exit = item->callback();
                }
            }
            static char result[TEXT_BUFFER_COLS];
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
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
        }

        if (pressed & BTN_DOWN) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item++;
        }
        if (pressed & BTN_UP) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item--;
        }

        sleep_ms(25);
    }
    if (manual_palette_selected > 0) {
        manual_assign_palette(palette16, manual_palette_selected);
    } else {
        char rom_title[16];
        auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
    }
    // a Game Boy Color game keeps its own palette in slots 0..63
    if (!gb.cgb.cgbMode)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                graphics_set_palette(i * 4 + j, RGB565_TO_RGB888(palette16[i][j]));
                palette[i][j] = i * 4 + j;
            }
    f_save_conf();
#if SOFTTV
    graphics_set_tv(tv_system == 0, true); // applied by graphics_set_mode()
#endif
#if VGA || HDMI
    graphics_set_scale((enum graphics_scale_t)screen_scale);
#endif
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}

int main() {
    detect_flash_size();
    overclock();
    /* QMI CS1 setup before core1 starts: the probe briefly blocks XIP */
    gb_psram_init();

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);
    // Do not draw before the video driver has its text buffer:
    // draw_text() into a NULL text_buffer is lost.
    while (!render_ready) tight_loop_contents();

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

    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        /// TODO: error handling
        while(1);
    } else {
        f_mkdir(HOME_DIR);
        f_load_conf();
#if SOFTTV
        graphics_set_tv(tv_system == 0, true); // applied by the next graphics_set_mode()
#endif
#if VGA || HDMI
        graphics_set_scale((enum graphics_scale_t)screen_scale);
#endif
    }

    while (true) {
        /* ROM File selector */
        if (FR_OK == fr) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            filebrowser("gbc,gb");
            graphics_set_mode(GRAPHICSMODE_DEFAULT);
        }

        /* Initialise GB context. */
        gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                      &gb_cart_ram_write, &gb_error, nullptr);

        if (ret != GB_INIT_NO_ERROR) {
            while (1) draw_text("error", 1, 1, 1, 2);
        }

        /* Automatically assign a colour palette to the game */
        if (!manual_palette_selected) {
            char rom_title[16];
            auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
        }
        else {
            manual_assign_palette(palette16, manual_palette_selected);
        }

        if (!gb.cgb.cgbMode)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++) {
                    graphics_set_palette(i * 4 + j, RGB565_TO_RGB888(palette16[i][j]));
                    palette[i][j] = i * 4 + j;
                }
        //palette[i][j] = convertRGB565toRGB222(palette16[i][j]);

        gb_init_lcd(&gb, &lcd_draw_line);
        /* Load Save File. */
        read_cart_ram_file(&gb);

        // the Enter/Start that launched the ROM must not reach the game
        input_gate_t game_gate;
        game_gate.arm();

        //=============================================================================
        while (!restart) {
            //------------------------------------------------------------------------------
            if (fxPressedV) {
                if (altPressed) {
                    save_slot = fxPressedV;
                    load();
                } else if (ctrlPressed) {
                    save_slot = fxPressedV;
                    save();
                }
                fxPressedV = 0; // once per key press, not on every frame
            }
            const uint8_t keys = game_gate.read();
            gb.direct.joypad_bits.up = !(keys & BTN_UP);
            gb.direct.joypad_bits.down = !(keys & BTN_DOWN);
            gb.direct.joypad_bits.left = !(keys & BTN_LEFT);
            gb.direct.joypad_bits.right = !(keys & BTN_RIGHT);
            gb.direct.joypad_bits.a = !(keys & BTN_A);
            gb.direct.joypad_bits.b = !(keys & BTN_B);
            gb.direct.joypad_bits.select = !(keys & BTN_SELECT);
            gb.direct.joypad_bits.start = !(keys & BTN_START);

            //gb.direct.joypad = nespad_state;
            //------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            static int keydown_counter = 0;
            if (!(gb.direct.joypad & 0b00001100) || nespad_state & DPAD_X) {
                char romname[24];

                gb_get_rom_name(&gb, romname);

                if (nullptr != strstr(romname, "ZELDA")) {
                    // half a second (the game itself uses Start+Select)
                    if (keydown_counter++ > 30) {
                        menu();
                        keydown_counter = 0;
                        game_gate.arm();
                    }
                }

                else {
                    menu();
                    // keys still held on leaving the menu (Enter on "Return to game",
                    // the Start+Select combination) must not reach the game
                    game_gate.arm();
                }
            }
            else {
                keydown_counter = 0; // the half second counts one continuous hold
            }
            // TODO F2
            if ((nespad_state & DPAD_RT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_RT) {
                    sleep_ms(500);
                }
                load();
            }

            // TODO F3
            if ((nespad_state & DPAD_LT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_LT) {
                    sleep_ms(500);
                }
                save();
            }

            //-----------------------------------------------------------------
            gb_run_frame(&gb);

            //gb.direct.interlace = 1;

            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
        }
        restart = false;
    }
}
