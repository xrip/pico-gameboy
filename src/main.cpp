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
#include <strings.h>

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
#include <pico/critical_section.h>
#include <sys/unistd.h>
#include <hardware/watchdog.h>
#include <hardware/clocks.h>

#include "audio.h"
#include "minigb_apu.h"
#if HWAY
#include "hway/ay_hway.h"
#endif

/* Project headers */
#include "hedley.h"
#include "peanut_gb.h"
#include "gbcolors.h"
#include "mono_palettes.h"
#include "gb_palettes16.h"

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
/* Time in base Game Boy CPU cycles since the start of the audio frame.
 * Shared with the C APU synthesizer; no extra field in gb_s/save states. */
extern "C" {
uint32_t gb_audio_cycles = 0;
}
/* True only after a successful gb_init().  A menu entered from the ROM
 * browser must never use a non-initialised (or failed) GB context. */
static bool gb_initialized = false;
/* Host heartbeat: used only by the F11 hang diagnostic.  1 = GB CPU,
 * 2 = audio, 0 = normal main loop.  Core 1 continues servicing USB while
 * core 0 is blocked, so F11 can report the last live GB state. */
static volatile uint32_t gb_debug_frame_id = 0;
static volatile uint8_t gb_debug_phase = 0;
/* A failed partial SD -> PSRAM/flash write may have damaged the ROM
 * underlying a still-running GB context.  Never resume it via F10. */
static bool rom_image_damaged = false;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
static FATFS fs;

uint16_t stream[AUDIO_BUFFER_SIZE_BYTES];

typedef uint32_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette

/* Colours of Game Boy (not Color) games, as in pico-wonderswan: each of the
 * three palettes (background, sprites 0, sprites 1 - 4 shades each) has its
 * own selector: AUTO / the GBC boot palettes (the colours this palette gets
 * there), the pico-gamate four-colour palettes, Random or Custom.  The palette
 * editor shows all 12 colours; editing one turns its palette into Custom.
 * One selector above them sets all three; they are chosen one by one only
 * when it is CUSTOM. */
enum palette_layer_e : uint8_t { LAYER_BG = 0, LAYER_OBJ0, LAYER_OBJ1, LAYER_COUNT };
/* the core (LCD_PALETTE_ALL) and gbcolors.h number them OBJ0, OBJ1, BG */
static const uint8_t layer_hw[LAYER_COUNT] = { 2, 0, 1 };
static constexpr uint8_t PALETTE_AUTO = 0;                               // by the game (GBC boot ROM)
static constexpr uint8_t PALETTE_MONO_FIRST = NUMBER_OF_MANUAL_PALETTES; // pico-gamate 4-colour palettes
static constexpr uint8_t PALETTE_RANDOM = PALETTE_MONO_FIRST + MONO_PALETTES_NUMBER;
static constexpr uint8_t PALETTE_CUSTOM = PALETTE_RANDOM + 1;
/* The palette numbers in config.ini and the game .ini files.  List 2 put the
 * Super Game Boy palettes after DMG: a file without "palette_list=2" numbers
 * every palette after DMG SGB_PALETTES_NUMBER lower. */
static constexpr unsigned PALETTE_LIST_VERSION = 2;
static uint8_t palette_from_list1(const uint8_t sel) {
    return sel >= PALETTE_MONO_FIRST ? (uint8_t)(sel + SGB_PALETTES_NUMBER) : sel;
}
/* the selector for all three palettes; with CUSTOM each has its own one */
static uint8_t palette_all = PALETTE_AUTO;
static uint8_t palette_index[LAYER_COUNT] = { PALETTE_AUTO, PALETTE_AUTO, PALETTE_AUTO };
static uint32_t pal_rgb[LAYER_COUNT][4];    // the colours in use, RGB888
static uint32_t random_rgb[LAYER_COUNT][4]; // what Random drew
static uint32_t custom_rgb[LAYER_COUNT][4] = {
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
};
/* User-supplied 4x4 shades (current shade * 4 + previous shade). */
static uint32_t custom_shades16[16] = {
    0xF0F0F0, 0xE0E0E0, 0xD0D0D0, 0xC0C0C0,
    0xB0B0B0, 0xA0A0A0, 0x909090, 0x808080,
    0x707070, 0x606060, 0x505050, 0x404040,
    0x303030, 0x202020, 0x101010, 0x000000
};
static bool game_ini_linked = false;
static char rom_file[80] = "";      // the ROM file name, for the game palette

/* ROM demonstration: same duration choices as pico-watara.  The playback
 * directory is kept stable while the browser is opened with F10. */
static bool demo_active = false;
static bool demo_requested = false;
static bool demo_advance_pending = false;
static uint64_t demo_game_started_at = 0;
static uint8_t demo_duration = 0;
static uint8_t demo_name = 1;      // show ROM title during the first 10 seconds
static uint8_t demo_timer_osd = 1; // keep the countdown visible for the entire demo
static char demo_dir[256] = "";
static char demo_current_name[80] = "";
static constexpr uint16_t demo_seconds[] = {15, 30, 45, 60, 120, 180, 300, 600};
static constexpr uint8_t PALETTE16_CUSTOM = GB_PALETTES16_COUNT + 1;
/* 0: 4-shade palette; 1..16: worksheet presets; 17: imported CUSTOM. */
static uint8_t palette16_select = 0;
static uint8_t palette16_global = 0;

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
static volatile int8_t kbd_hex_key = -1;     // a hex digit just pressed (palette editor)
static volatile bool kbd_hex_mode = false;   // the palette editor is open: A-F, W, S are digits
static volatile uint8_t palette_layer_random_requested = 0;   // F1..F3: BG / OBJ0 / OBJ1
static volatile bool palette_shuffle_requested = false;       // F7: generate + link to ROM
static volatile bool palette_shuffle_requested_no_save = false; // F6: generate only
static volatile bool palette_save_requested = false;            // F8
static volatile bool palette_default_requested = false;         // F9
static volatile bool palette_editor_requested = false;          // F12
static volatile bool filebrowser_toggle_requested = false;     // F10: game -> browser -> settings -> game

/* R / P in the ROM browser arm a recording or its playback for the selected
 * cartridge.  Keyboard reports are produced on core 1. */
static volatile bool replay_browser_active = false;
static volatile uint8_t replay_browser_command = 0;
enum replay_launch_t : uint8_t { REPLAY_LAUNCH_NORMAL, REPLAY_LAUNCH_RECORD, REPLAY_LAUNCH_PLAY };
static replay_launch_t replay_launch = REPLAY_LAUNCH_NORMAL;

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /*    printf("HID key report modifiers %2.2X report ", report->modifier);
        for (unsigned char i: report->keycode)
            printf("%2.2X", i);
        printf("\r\n");*/
#if VGA || HDMI || SOFTTV
    if (render_ready && isInReport(report, HID_KEY_F11) &&
        !isInReport(prev_report, HID_KEY_F11)) {
        char line[53];
        snprintf(line, sizeof line, "DBG %lu P%u PC=%04X IE=%02X IF=%02X L=%02X H%u",
                 (unsigned long)gb_debug_frame_id, (unsigned)gb_debug_phase,
                 (unsigned)gb.cpu_reg.pc.reg, (unsigned)gb.hram_io[0xFF],
                 (unsigned)gb.hram_io[0x0F], (unsigned)gb.hram_io[0x40],
                 (unsigned)gb.gb_halt);
        graphics_set_overlay(line, true);
    }
#endif
    if (replay_browser_active) {
        if (isInReport(report, HID_KEY_R) && !isInReport(prev_report, HID_KEY_R))
            replay_browser_command = 'R';
        if (isInReport(report, HID_KEY_P) && !isInReport(prev_report, HID_KEY_P))
            replay_browser_command = 'P';
    }
    keyboard.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    keyboard.a = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_KEYPAD_0);
    keyboard.b = isInReport(report, HID_KEY_X) || (!replay_browser_active && isInReport(report, HID_KEY_P)) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);

    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    const bool wasd = !kbd_hex_mode;
    keyboard.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || (wasd && isInReport(report, HID_KEY_W)) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || (wasd && isInReport(report, HID_KEY_S)) || isInReport(report, HID_KEY_KEYPAD_2) || isInReport(report, HID_KEY_KEYPAD_5);
    keyboard.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || (wasd && isInReport(report, HID_KEY_A)) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || (wasd && isInReport(report, HID_KEY_D)) || isInReport(report, HID_KEY_KEYPAD_6);

    // hex digits for the palette editor: 0-9 of the main block, A-F
    if (kbd_hex_mode) {
        static const uint8_t hex_keys[16] = {
            HID_KEY_0, HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5, HID_KEY_6, HID_KEY_7,
            HID_KEY_8, HID_KEY_9, HID_KEY_A, HID_KEY_B, HID_KEY_C, HID_KEY_D, HID_KEY_E, HID_KEY_F
        };
        for (int8_t h = 0; h < 16; h++)
            if (isInReport(report, hex_keys[h]) && !isInReport(prev_report, hex_keys[h]))
                kbd_hex_key = h;
    }

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
    else {
        /* F1..F3 regenerate BG / OBJ0 / OBJ1 separately; F6 regenerates all
         * three, F7 regenerates and links them to the ROM, F8 links the
         * currently displayed colours, F9 resets AUTO, F12 opens the editor. */
        static const uint8_t layer_keys[LAYER_COUNT] = { HID_KEY_F1, HID_KEY_F2, HID_KEY_F3 };
        for (uint8_t layer = 0; layer < LAYER_COUNT; layer++)
            if (isInReport(report, layer_keys[layer]) && !isInReport(prev_report, layer_keys[layer]))
                palette_layer_random_requested = layer + 1;
        if (isInReport(report, HID_KEY_F6) && !isInReport(prev_report, HID_KEY_F6))
            palette_shuffle_requested_no_save = true;
        if (isInReport(report, HID_KEY_F7) && !isInReport(prev_report, HID_KEY_F7))
            palette_shuffle_requested = true;
        if (isInReport(report, HID_KEY_F8) && !isInReport(prev_report, HID_KEY_F8))
            palette_save_requested = true;
        if (isInReport(report, HID_KEY_F9) && !isInReport(prev_report, HID_KEY_F9))
            palette_default_requested = true;
        if (isInReport(report, HID_KEY_F12) && !isInReport(prev_report, HID_KEY_F12))
            palette_editor_requested = true;
        if (isInReport(report, HID_KEY_F10) && !isInReport(prev_report, HID_KEY_F10))
            filebrowser_toggle_requested = true;
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

/* The picture is triple buffered.  The emulator draws a whole frame in a few
 * milliseconds and then waits for the sound, while the video driver shows the
 * buffer at its own rate: with a single buffer the screen showed a mix of the
 * old and the new frame (sprites of the new frame over the background of the
 * old one, or the other way round).  Now the emulator draws into the back
 * buffer, a complete frame becomes pending, and the render core shows it in
 * the vertical blank of the display together with its palette.  A frame still
 * pending when the next one is complete is dropped, so the emulator never
 * waits for the display.  SCREEN is buffer 0: the text mode and the flash
 * loader use it as before. */
static uint8_t SCREEN_2[2][LCD_HEIGHT][LCD_WIDTH];
static uint8_t (*const frames[3])[LCD_WIDTH] = {SCREEN, SCREEN_2[0], SCREEN_2[1]};

/* LCD ghosting: the Game Boy screen is slow, a pixel still shows about half
 * of the previous frame.  Games use it for transparency: a sprite drawn every
 * second frame is half-transparent there, while a fast screen shows it
 * flickering.  So a Game Boy (not Color) frame is shown mixed 1:1 with the
 * previous one: slots 12..143 hold the transitions between two different
 * colours of the slots 0..11, one slot per ordered (previous, current) pair:
 * a 16 shades preset has different colours for A->B and B->A (a Game Boy
 * Color game has its own palette there). */
static uint8_t lcd_ghosting = 1;
static constexpr uint8_t DMG_SLOTS = 12;
static uint8_t blend_slot[DMG_SLOTS][DMG_SLOTS]; // [previous][current]
static uint8_t blend_prev[LCD_HEIGHT][LCD_WIDTH / 2]; // the previous frame, slots 0..11 (4 bits each)

static void blend_slots_init() {
    uint8_t next = DMG_SLOTS;
    for (uint8_t a = 0; a < DMG_SLOTS; a++) {
        for (uint8_t b = 0; b < DMG_SLOTS; b++)
            blend_slot[a][b] = a == b ? a : next++;
    }
}

#if VGA || HDMI || SOFTTV
/* Watara-style OSD: the scanout driver owns the bottom 12 screen lines.
 * A double-buffered string can be updated from core 0 during gameplay. */
static void demo_draw_title_overlay() {
    static unsigned last_seconds = 0xffffffffu;
    static bool last_show_title = false;
    static bool last_show_timer = false;
    static bool visible = false;
    static uint64_t last_game_started_at = 0;

    if (!demo_active || !demo_game_started_at) {
        if (visible) graphics_set_overlay(nullptr, false);
        visible = false;
        last_seconds = 0xffffffffu;
        last_show_title = false;
        last_show_timer = false;
        last_game_started_at = 0;
        return;
    }

    const uint8_t duration = demo_duration < count_of(demo_seconds) ? demo_duration : 0;
    const uint64_t total_us = (uint64_t)demo_seconds[duration] * 1000000ull;
    const uint64_t elapsed_us = time_us_64() - demo_game_started_at;
    const uint64_t remain_us = elapsed_us < total_us ? total_us - elapsed_us : 0;
    const unsigned seconds = (unsigned)((remain_us + 999999ull) / 1000000ull);
    const bool show_title = demo_name && demo_current_name[0] && elapsed_us < 10000000ull;
    const bool show_timer = demo_timer_osd != 0;

    // The title and countdown are independent. No active component means
    // no bar, but turning off the title never turns off the countdown.
    if (!show_title && !show_timer) {
        if (visible) graphics_set_overlay(nullptr, false);
        visible = false;
        last_seconds = 0xffffffffu;
        last_show_title = false;
        last_show_timer = false;
        return;
    }
    if (last_game_started_at != demo_game_started_at) {
        last_game_started_at = demo_game_started_at;
        last_seconds = 0xffffffffu;
    }
    if (visible && seconds == last_seconds && show_title == last_show_title &&
        show_timer == last_show_timer)
        return;

    char label[53];
    if (show_title) {
        char title[45];
        const char* dot = strrchr(demo_current_name, '.');
        size_t n = dot ? (size_t)(dot - demo_current_name) : strlen(demo_current_name);
        if (n >= sizeof title) n = sizeof title - 1;
        for (size_t i = 0; i < n; ++i)
            title[i] = demo_current_name[i] == '_' ? ' ' : demo_current_name[i];
        title[n] = '\0';
        if (show_timer)
            snprintf(label, sizeof label, "%s  %us", title, seconds);
        else
            snprintf(label, sizeof label, "%s", title);
    } else {
        snprintf(label, sizeof label, "%us", seconds);
    }
    graphics_set_overlay(label, true);
    visible = true;
    last_seconds = seconds;
    last_show_title = show_title;
    last_show_timer = show_timer;
}
#else
/* TFT/legacy output has no scanout OSD: retain the original LCD fallback. */
static void demo_overlay_line(const unsigned y, uint8_t* line) {
    if (!demo_active || y < LCD_HEIGHT - 8) return;
    const uint64_t elapsed = time_us_64() - demo_game_started_at;
    const bool show_title = demo_name && demo_current_name[0] && elapsed < 10000000ull;
    const bool show_timer = demo_timer_osd != 0;
    if (!show_title && !show_timer) return;
    static char label[27];
    if (y == LCD_HEIGHT - 8) {
        const uint8_t duration = demo_duration < count_of(demo_seconds) ? demo_duration : 0;
        const uint64_t length = (uint64_t)demo_seconds[duration] * 1000000ull;
        const unsigned left = (unsigned)((elapsed < length ? (length - elapsed + 999999ull) : 0) / 1000000ull);
        if (show_title) {
            char title[19];
            unsigned n = 0;
            while (n + 1 < sizeof title && demo_current_name[n] && demo_current_name[n] != '.') {
                const char c = demo_current_name[n];
                title[n] = c == '_' ? ' ' : c;
                n++;
            }
            title[n] = '\0';
            if (show_timer) snprintf(label, sizeof label, "%s %us", title, left);
            else snprintf(label, sizeof label, "%s", title);
        } else {
            snprintf(label, sizeof label, "%us", left);
        }
    }
    memset(line, 193, LCD_WIDTH);
    const unsigned font_row = y - (LCD_HEIGHT - 8);
    for (unsigned x = 0; label[x] && x < LCD_WIDTH / 6; x++) {
        const uint8_t bits = font_6x8[(uint8_t)label[x] * 8u + font_row];
        for (unsigned b = 0; b < 6; b++)
            if (bits & (1u << b)) line[x * 6 + b] = 192;
    }
}
#endif

/* Game Boy Color palettes often change in the middle of a frame (the status
 * bar under the play field gets its own colours).  The buffers keep 8-bit
 * colour indices, and the video driver turns them into colours while it shows
 * the frame: the lines drawn before a mid-frame change keep their set, and the
 * following lines use the next one: up to 3 sets of 64 colours, palette slots
 * 0..191.  Every frame keeps its own copy of the sets; they go to the video
 * driver when the frame is shown, so the frame on the screen never changes its
 * colours.
 * "Hi-colour" pictures change the palettes on almost every line (thousands of
 * colours in a frame).  A frame that needs more than 3 sets is drawn in a
 * fixed 6x6x5 colour cube instead (slots 0..179, 2x2 ordered dither), with the
 * colours each line had when it was drawn. */
#define CGB_PAL_SETS 3
#define CGB_PAL_SIZE (CGB_PAL_SETS * 64)
#define CGB_CUBE 0xff // frame_sets[]: the frame uses the colour cube
#define CGB_CUBE_SIZE (6 * 6 * 5)
static uint32_t frame_pal[3][CGB_PAL_SIZE];
static uint8_t frame_sets[3]; // sets the frame uses, 0 for a Game Boy frame
static uint32_t cube_pal[CGB_CUBE_SIZE];

static void cube_pal_init() {
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 5; b++)
                cube_pal[(r * 6 + g) * 5 + b] = RGB888(r * 255 / 5, g * 255 / 5, b * 255 / 4);
}

static critical_section_t frame_lock;
static volatile int8_t frame_front = 0;    // on the screen (render core)
static volatile int8_t frame_pending = -1; // complete, to be shown
static int8_t frame_back = 1;              // being drawn (emulator core)

static uint32_t hw_pal[CGB_PAL_SIZE];      // what the video driver holds (render core)
static volatile bool hw_pal_invalid = true;

/* emulator core: the back buffer holds a complete frame */
static void frame_complete() {
    critical_section_enter_blocking(&frame_lock);
    const int8_t done = frame_back;
    const int8_t dropped = frame_pending;
    frame_pending = done;
    frame_back = dropped >= 0 ? dropped : 3 - frame_front - done;
    critical_section_exit(&frame_lock);
}

/* render core, in the vertical blank: show the pending frame */
static void __time_critical_func(frame_show)() {
    critical_section_enter_blocking(&frame_lock);
    const int8_t p = frame_pending;
    if (p >= 0) {
        frame_front = p;
        frame_pending = -1;
    }
    critical_section_exit(&frame_lock);
    if (p < 0) return;
    // the emulator core writes neither this buffer nor its palette now
    if (frame_sets[p]) {
        if (hw_pal_invalid) {
            hw_pal_invalid = false;
            memset(hw_pal, 0xff, sizeof hw_pal); // no colour has all bits set
        }
        const bool cube = frame_sets[p] == CGB_CUBE;
        const uint32_t* pal = cube ? cube_pal : frame_pal[p];
        const unsigned int n = cube ? CGB_CUBE_SIZE : frame_sets[p] * 64u;
        for (unsigned int i = 0; i < n; i++) {
            if (hw_pal[i] != pal[i]) {
                hw_pal[i] = pal[i];
                graphics_set_palette(i, pal[i]);
            }
        }
    }
    graphics_set_buffer((uint8_t *)frames[p], LCD_WIDTH, LCD_HEIGHT);
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();
#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
#endif
    graphics_init();
#if !(VGA || HDMI || SOFTTV)
    // Framebuffer-only fallback (TFT): no scanout OSD and no backplane.
    graphics_set_palette(192, RGB888(255, 255, 255)); // Demo text
    graphics_set_palette(193, RGB888(0, 0, 0));       // Demo label background
#endif

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
#ifndef TFT
    uint32_t frame_count = graphics_frame_count;
#endif
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            frame_show();
            refresh_lcd();
            last_renderer_tick = tick;
        }
#else
        if (frame_count != graphics_frame_count) {
            frame_count = graphics_frame_count; // the vertical blank of the display
            frame_show();
        }
#endif
#if HWAY
        hway_poll(); // the AY bus belongs to this core
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
/* A game rewrites a palette in a burst of writes that may span up to 4 lines;
 * the whole burst goes into one set (the lines drawn during it show its final
 * colours).  Writes in the vertical blank need nothing: every frame starts
 * with the current colours in set 0. */
static uint8_t cgb_set = 0;                       // the set for the next lines
static uint8_t cgb_need = 1;                      // sets the frame would need
static bool cgb_set_used = false;                 // a line was drawn with it
static uint8_t cgb_last_y = LCD_HEIGHT - 1;
static int16_t cgb_burst_y = -16;                 // line where the last burst began
static bool frame_started = false;                // line 0 is in the back buffer
static bool cgb_cube = false;                     // the frame is drawn in the cube
static bool cgb_cube_next = false;                // so is the next one
static uint16_t cgb_raw[CGB_PAL_SETS][64];        // the sets as GBC colours
static uint8_t cube_q[4][64];                     // cube colour per dither position

static uint16_t cgb_raw_color(const uint8_t i) {
    const uint8_t* const p = i < 32 ? &gb.cgb.BGPalette[i * 2] : &gb.cgb.OAMPalette[(i - 32) * 2];
    return p[0] | p[1] << 8; // bits 0-4 red, 5-9 green, 10-14 blue
}

/* 2x2 ordered dither: position (x & 1) + (y & 1) * 2 */
static inline uint8_t cube_index(const uint16_t c, const unsigned int pos) {
    static const uint8_t bayer[4] = {0, 2, 3, 1};
    const unsigned int k = 2 * bayer[pos] + 1; // threshold (k / 8) of a step
    const unsigned int r = (c & 31) * 40 + k * 31;
    const unsigned int g = (c >> 5 & 31) * 40 + k * 31;
    const unsigned int b = (c >> 10 & 31) * 32 + k * 31;
    return (r / 248 * 6 + g / 248) * 5 + b / 248;
}

static void cube_set_color(const uint8_t i) {
    const uint16_t c = cgb_raw_color(i);
    for (unsigned int pos = 0; pos < 4; pos++)
        cube_q[pos][i] = cube_index(c, pos);
}

static void cgb_load_set(const uint8_t set) {
    memcpy(&frame_pal[frame_back][set * 64], gb.cgb.fixPalette, sizeof gb.cgb.fixPalette);
    for (uint8_t i = 0; i < 64; i++)
        cgb_raw[set][i] = cgb_raw_color(i);
}

/* a 4th set is needed: the lines drawn so far go into the cube too */
static void cgb_enter_cube() {
    static uint8_t q[4][CGB_PAL_SIZE];
    for (unsigned int v = 0; v < (cgb_set + 1u) * 64; v++)
        for (unsigned int pos = 0; pos < 4; pos++)
            q[pos][v] = cube_index(cgb_raw[v >> 6][v & 63], pos);
    for (unsigned int y = 0; y <= cgb_last_y; y++) {
        uint8_t* const line = frames[frame_back][y];
        const uint8_t* const q0 = q[(y & 1) * 2];
        const uint8_t* const q1 = q[(y & 1) * 2 + 1];
        for (unsigned int x = 0; x < LCD_WIDTH; x += 2) {
            line[x] = q0[line[x]];
            line[x + 1] = q1[line[x + 1]];
        }
    }
    for (uint8_t i = 0; i < 64; i++)
        cube_set_color(i);
    cgb_cube = true;
}

/* a new game, a loaded state, the menu: the video driver gets all colours again */
static void cgb_palette_reset() {
    cgb_set = 0;
    cgb_need = 1;
    cgb_set_used = false;
    cgb_last_y = LCD_HEIGHT - 1;
    cgb_burst_y = -16;
    cgb_cube = cgb_cube_next = false;
    hw_pal_invalid = true;
}

void gb_cgb_palette_changed(const uint8_t index) {
    if (cgb_last_y >= LCD_HEIGHT - 1)
        return; // vertical blank (or LCD off after a full frame)
    if (cgb_last_y > cgb_burst_y + 3 && cgb_set_used) { // a new burst
        cgb_burst_y = cgb_last_y;
        cgb_set_used = false;
        if (cgb_need < 255) cgb_need++;
        if (!cgb_cube) {
            if (cgb_set < CGB_PAL_SETS - 1) {
                cgb_set++; // keep the lines already drawn in their colours
                cgb_load_set(cgb_set);
                return;
            }
            cgb_enter_cube();
            return;
        }
    }
    if (cgb_cube)
        cube_set_color(index);
    else {
        frame_pal[frame_back][cgb_set * 64 + index] = gb.cgb.fixPalette[index];
        cgb_raw[cgb_set][index] = cgb_raw_color(index);
    }
}

void __always_inline lcd_draw_line(struct gb_s* gb, const uint8_t pixels[160], const uint_fast8_t y) {
    uint8_t* const line = frames[frame_back][y];
    if (y == 0) frame_started = true;
    if (gb->cgb.cgbMode) {
        if (y == 0) {
            cgb_burst_y = -16;
            cgb_set = 0;
            cgb_need = 1;
            cgb_cube = cgb_cube_next;
            if (cgb_cube) {
                for (uint8_t i = 0; i < 64; i++)
                    cube_set_color(i);
            }
            else
                cgb_load_set(0);
        }
        cgb_last_y = y;
        cgb_set_used = true;
        if (cgb_cube) {
            const uint8_t* const q0 = cube_q[(y & 1) * 2];
            const uint8_t* const q1 = cube_q[(y & 1) * 2 + 1];
            for (unsigned int x = 0; x < LCD_WIDTH; x += 2) {
                line[x] = q0[pixels[x]];
                line[x + 1] = q1[pixels[x + 1]];
            }
        }
        else if (cgb_set == 0) {
            memcpy((uint32_t *)line, (uint32_t *)pixels, 160);
        }
        else {
            const uint8_t base = cgb_set * 64;
            for (unsigned int x = 0; x < LCD_WIDTH; x++)
                line[x] = pixels[x] + base;
        }
    }
    else if (lcd_ghosting) {
        uint8_t* const prev = blend_prev[y];
        for (unsigned int x = 0; x < LCD_WIDTH; x += 2) {
            const uint8_t c0 = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            const uint8_t c1 = palette[(pixels[x + 1] & LCD_PALETTE_ALL) >> 4][pixels[x + 1] & 3];
            const uint8_t p = prev[x / 2];
            line[x] = blend_slot[p & 15][c0];
            line[x + 1] = blend_slot[p >> 4][c1];
            prev[x / 2] = c0 | c1 << 4;
        }
    }
    else {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
            line[x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    }
#if !(VGA || HDMI || SOFTTV)
    demo_overlay_line(y, line);
#endif
    if (y == LCD_HEIGHT - 1 && frame_started) {
        frame_started = false;
        if (gb->cgb.cgbMode) {
            frame_sets[frame_back] = cgb_cube ? CGB_CUBE : cgb_set + 1;
            cgb_cube_next = cgb_need > CGB_PAL_SETS; // the next frame is likely the same
        }
        else
            frame_sets[frame_back] = 0;
        frame_complete();
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

    // A read/write failure after this point leaves the old image unusable.
    rom_image_damaged = true;
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

    {
        const char* name = pathname;
        for (const char* q = pathname; *q; q++)
            if (*q == '\\' || *q == '/') name = q + 1;
        snprintf(rom_file, sizeof rom_file, "%s", name);
    }
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
    rom_image_damaged = false;
    return true;
}

/* Where the browser was left: re-entering it opens the same directory with
 * the same entry under the cursor (e.g. the ROM that was just played). */
static char fb_dir[256] = "\\GB";
static bool fb_menu_requested = false; // START+SELECT in the browser
static char fb_focus[80] = "";
static bool fb_can_cancel = false;    // browser entered from a running game
static bool fb_canceled = false;      // F10 closed browser: resume same GB context
static bool fb_f10_menu = false;      // ... after the settings menu (F10 cycle)
static bool fb_rom_ready = false;     // selected ROM / previous ROM, not Demo

static void fb_remember(const char* dir, const char* name) {
    strncpy(fb_dir, dir, sizeof fb_dir - 1);
    fb_dir[sizeof fb_dir - 1] = '\0';
    strncpy(fb_focus, name, sizeof fb_focus - 1);
    fb_focus[sizeof fb_focus - 1] = '\0';
}

/* Check Demo candidates on SD before replacing the current ROM in
 * PSRAM/Flash.  Peanut-GB rejects a bad header checksum or unsupported
 * cartridge; such images must not terminate the entire demo carousel. */
static bool demo_rom_header_ok(const char* pathname) {
    static FIL file;
    static uint8_t header[0x150]; // keep core 0's stack small
    UINT received = 0;
    if (f_open(&file, pathname, FA_READ) != FR_OK) return false;
    const FSIZE_t file_bytes = f_size(&file);
    const FRESULT read_result = f_read(&file, header, sizeof header, &received);
    const FRESULT close_result = f_close(&file);
    if (read_result != FR_OK || close_result != FR_OK || received != sizeof header)
        return false;

    uint8_t check = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i)
        check = (uint8_t)(check - header[i] - 1);
    if (check != header[0x14d] || header[0x148] > 8 ||
        header[0x149] > 5 || file_bytes < (32768u << header[0x148]))
        return false;

    /* Same accepted cartridge types as the cart_mbc table in peanut_gb.h. */
    static const uint8_t supported[31] = {
        1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1,
        1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1
    };
    const uint8_t cart = header[0x147];
    return cart < sizeof supported && supported[cart];
}

/* The demo visits playable ROMs in the directory where it was launched.
 * It chooses lexicographically, without heap allocation or a large list. */
static bool demo_load_next_rom(const char* after) {
    char cursor[80] = "";
    if (after) snprintf(cursor, sizeof cursor, "%s", after);
    while (true) {
        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, demo_dir) != FR_OK) return false;
        char next[80] = "";
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (info.fattrib & AM_DIR) continue;
            if (strlen(info.fname) >= sizeof next || !isExecutable(info.fname, "gbc,gb") ||
                !rom_fits(info.fsize) || (cursor[0] && strcmp(info.fname, cursor) <= 0))
                continue;
            if (!next[0] || strcmp(info.fname, next) < 0)
                snprintf(next, sizeof next, "%s", info.fname);
        }
        f_closedir(&dir);
        if (!next[0]) return false;
        char path[256];
        if (snprintf(path, sizeof path, "%s\\%s", demo_dir, next) < (int)sizeof path &&
            demo_rom_header_ok(path) && filebrowser_loadfile(path) &&
            gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                    &gb_cart_ram_write, &gb_error, nullptr) == GB_INIT_NO_ERROR) {
            snprintf(demo_current_name, sizeof demo_current_name, "%s", next);
            demo_game_started_at = time_us_64();
            return true;
        }
        snprintf(cursor, sizeof cursor, "%s", next); // skip failed ROM
    }
}

static bool demo_begin() {
    snprintf(demo_dir, sizeof demo_dir, "%s", fb_dir);
    demo_current_name[0] = '\0';
    demo_active = true;
    demo_advance_pending = false;
    if (demo_load_next_rom(nullptr)) return true;
    // A directory consisting of subdirectories cannot start a demo there;
    // try the regular ROM root as well, without changing the browser's path.
    if (strcmp(demo_dir, HOME_DIR) != 0) {
        snprintf(demo_dir, sizeof demo_dir, "%s", HOME_DIR);
        if (demo_load_next_rom(nullptr)) return true;
    }
    demo_active = false;
    return false;
}

/* Returns when a ROM is ready to run: a newly loaded one (START) or the
 * previous one (SELECT). */
static bool gbbp_try_install(const char* pathname); // a .gbbp file: install it as the backplane

enum palette_import_result_e { PAL_IMPORT_4, PAL_IMPORT_12, PAL_IMPORT_16, PAL_IMPORT_FORMAT, PAL_IMPORT_IO };
static palette_import_result_e import_palette_file(const char* pathname);
static bool is_palette_text_file(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".csv") || !strcasecmp(dot, ".hex"));
}

void filebrowser(const char* executables) {
    input_gate_t gate;
    char basepath[256];
    char focus[80];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, fb_dir);
    strcpy(focus, fb_focus);
    constexpr int per_page = TEXTMODE_ROWS - 3;
    replay_browser_command = 0;
    replay_browser_active = true;
    replay_launch = REPLAY_LAUNCH_NORMAL;

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
        draw_text(" Run ", off, 29, 0, 3);
        off += 5;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Prev ", off, 29, 0, 3);
        off += 6;
        draw_text("B", off, 29, 7, 0);
        off += 1;
        draw_text(" Demo ", off, 29, 0, 3);
        off += 6;
#ifndef TFT
        if (off + 15 <= TEXTMODE_COLS) {
            off++;
            draw_text("R", off++, 29, 7, 0);
            draw_text(" REC ", off, 29, 0, 3); off += 5;
            draw_text("P", off++, 29, 7, 0);
            draw_text(" PLAY ", off, 29, 0, 3); off += 6;
        }
        if (off + 10 <= TEXTMODE_COLS) { // F10: the settings (the F10 cycle)
            off++;
            draw_text("F10", off, 29, 7, 0); off += 3;
            draw_text(" Menu ", off, 29, 0, 3);
        }
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
                (is_palette_text_file(fileInfo.fname) ||
                 (rom_fits(fileInfo.fsize) && isExecutable(fileInfo.fname, executables)));
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
            const uint8_t replay_command = replay_browser_command;
            replay_browser_command = 0;
            if (replay_command && total_files > 0) {
                const file_item_t item = fileItems[offset + current_item];
                const char* dot = strrchr(item.filename, '.');
                if (!item.is_directory && item.is_executable && dot &&
                    (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"))) {
                    char fullpath[256];
                    if (snprintf(fullpath, sizeof fullpath, "%s\\%s", basepath, item.filename) < (int)sizeof fullpath &&
                        filebrowser_loadfile(fullpath)) {
                        fb_remember(basepath, item.filename);
                        fb_rom_ready = true;
                        replay_launch = replay_command == 'R' ? REPLAY_LAUNCH_RECORD : REPLAY_LAUNCH_PLAY;
                        replay_browser_active = false;
                        return;
                    }
                    strcpy(focus, item.filename);
                    break; // redraw after the ROM loader's status window
                }
            }

            /* F10 cycles game -> browser -> settings -> game; without a
             * running game: browser -> settings -> browser. */
            if (filebrowser_toggle_requested) {
                filebrowser_toggle_requested = false;
                fb_remember(basepath, total_files ? fileItems[offset + current_item].filename : "");
                replay_browser_active = false;
                if (fb_can_cancel) {
                    fb_canceled = true; // the game goes on after the menu
                    fb_f10_menu = true;
                } else {
                    fb_menu_requested = true;
                    fb_rom_ready = false;
                }
                return;
            }

            // B starts Demo; the browser never tries to boot a ROM itself.
            if (keys & BTN_B) {
                fb_remember(basepath, total_files ? fileItems[offset + current_item].filename : "");
                demo_requested = true;
                fb_rom_ready = false;
                replay_browser_active = false;
                return;
            }

            // START+SELECT opens settings, independently of whether a ROM exists.
            // SELECT alone still runs the previous cartridge.
            const uint8_t held = input_held();
            if ((held & (BTN_START | BTN_SELECT)) == (BTN_START | BTN_SELECT) &&
                (keys & (BTN_START | BTN_SELECT))) {
                fb_menu_requested = true;
                fb_rom_ready = false;
                fb_remember(basepath, total_files ? fileItems[offset + current_item].filename : "");
                replay_browser_active = false;
                return;
            }
            if (keys & BTN_SELECT) {
                if (rom_image_damaged) {
                    draw_text("Previous ROM incomplete; load another", 1, 1, 12, 0);
                    sleep_ms(1200);
                    break; // redraw the browser, not a broken cartridge
                }
                fb_menu_requested = false;
                fb_remember(basepath, total_files ? fileItems[offset + current_item].filename : "");
                rom = previous_rom();
                fb_rom_ready = true;
                replay_browser_active = false;
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
                    const bool path_ok = snprintf(fullpath, sizeof fullpath, "%s\\%s", basepath,
                                                  file_at_cursor.filename) < (int)sizeof fullpath;
                    if (path_ok && is_palette_text_file(file_at_cursor.filename)) {
                        const int w = TEXTMODE_COLS < 38 ? TEXTMODE_COLS : 38;
                        const int x = (TEXTMODE_COLS - w) / 2;
                        const int y = (TEXTMODE_ROWS - 5) / 2;
                        draw_window("Import palette", x, y, w, 5);
                        draw_text("Replacing current CUSTOM palettes", x + 1, y + 2, 13, 1);
                        sleep_ms(60); // show replacement notice, no confirmation
                        const palette_import_result_e result = import_palette_file(fullpath);
                        const char* message = result == PAL_IMPORT_4 ? "4 colours x 3 CUSTOM replaced" :
                            result == PAL_IMPORT_12 ? "12 CUSTOM colours replaced" :
                            result == PAL_IMPORT_16 ? "16 shades CUSTOM replaced" :
                            result == PAL_IMPORT_FORMAT ? "Unexpected format" : "Cannot read palette file";
                        draw_window("Import palette", x, y, w, 5); // clear the earlier notice
                        draw_text(message, x + 1, y + 2,
                                  result == PAL_IMPORT_4 || result == PAL_IMPORT_12 || result == PAL_IMPORT_16 ? 10 : 13, 1);
                        sleep_ms(1300);
                        strcpy(focus, file_at_cursor.filename);
                        break; // redraw browser; never treat a palette as a ROM
                    }
                    if (path_ok && gbbp_try_install(fullpath)) {
                        // its message covered the list: redraw it, same entry
                        strcpy(focus, file_at_cursor.filename);
                        break;
                    }
                    if (path_ok && filebrowser_loadfile(fullpath)) {
                        fb_remember(basepath, file_at_cursor.filename);
                        fb_rom_ready = true;
                        replay_browser_active = false;
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
    START_DEMO,
    GAME_INI,
    PALETTE_EDITOR,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char* text;
    menu_type_e type;
    const void* value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[PALETTE_CUSTOM + 1][18];
} MenuItem;

static int save_slot = 0;
#ifndef CPU_FREQ
#define CPU_FREQ 378
#endif
#define FREQ_STR_(x) #x
#define FREQ_STR(x) FREQ_STR_(x)
#if CPU_FREQ >= 504
static uint16_t frequencies[] = { CPU_FREQ, 528 };
#else
static uint16_t frequencies[] = { CPU_FREQ, 396, 404, 408, 412, 416, 420, 424, 433 };
#endif
static uint8_t frequency_index = 0;

bool overclock() {
#if PICO_RP2350
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_FREQ > 378 ? VREG_VOLTAGE_1_70 : VREG_VOLTAGE_1_60);
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

/* The configuration, the game palettes and the saved states live in
 * /.config/gbc, apart from the games (as in pico-gamate); the settings are
 * kept per board and video output. */
#define CONFIG_DIR "/.config/gbc"

static const char* config_platform_name() {
#ifdef PICO_RP2350
    if (strcmp(PICO_BOARD, "murmulator2") == 0) return "m2p2";
    if (strcmp(PICO_BOARD, "olimex-pico-pc") == 0) return "pcp2";
    if (strcmp(PICO_BOARD, "waveshare_rp2350_pizero") == 0) return "z0p2";
    return "m1p2";
#else
    if (strcmp(PICO_BOARD, "murmulator2") == 0) return "m2p1";
    if (strcmp(PICO_BOARD, "olimex-pico-pc") == 0) return "pcp1";
    if (strcmp(PICO_BOARD, "waveshare_rp2040_pizero") == 0) return "z0p1";
    return "m1p1";
#endif
}

static const char* config_video_name() {
#if TFT
    return "tft";
#elif HDMI
    return "hdmi";
#elif TV
    return "tv";
#elif SOFTTV
    return "softtv";
#else
    return "vga";
#endif
}

static void config_mkdirs() {
    char path[64];
    f_mkdir("/.config"); // nothing happens if it is already there
    f_mkdir(CONFIG_DIR);
    snprintf(path, sizeof path, CONFIG_DIR "/%s", config_platform_name());
    f_mkdir(path);
    snprintf(path, sizeof path, CONFIG_DIR "/%s/%s", config_platform_name(), config_video_name());
    f_mkdir(path);
}

static void state_path(char* pathname, size_t size, const char* dir) {
    char filename[24];
    gb_get_rom_name(&gb, filename);
    if (save_slot)
        snprintf(pathname, size, "%s/%s_%d.save", dir, filename, save_slot);
    else
        snprintf(pathname, size, "%s/%s.save", dir, filename);
}

/* A Watara-style input movie: one event whenever the joypad changes, indexed
 * by emulated frame, not wall time.  No allocation and no SD I/O for unchanged
 * inputs.  A state contains the recording's session ID and current file offset:
 * loading that state truncates the movie and starts a new branch.  The first
 * 32 KiB of cartridge RAM are stored with the movie for deterministic replay.
 * A .gbr file is intentionally not binary-compatible with Watara .wvr. */
static constexpr uint32_t REPLAY_MAGIC = 0x31524247u;       // GBR1
static constexpr uint32_t REPLAY_STATE_MAGIC = 0x31534247u; // GBS1
struct __attribute__((packed)) replay_header_t {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t rom_bytes;
    uint32_t rom_tag;
    uint64_t session_id;
    uint32_t total_frames;
    char rom_name[24];
};
struct __attribute__((packed)) replay_event_t {
    uint32_t frame;
    uint8_t input;
};
struct __attribute__((packed)) replay_state_t {
    uint32_t magic;
    uint64_t session_id;
    uint32_t frame;
    uint32_t file_offset;
    uint8_t input;
    uint8_t reserved[3];
};
enum replay_mode_t : uint8_t { REPLAY_OFF, REPLAY_RECORD, REPLAY_PLAY };
static replay_mode_t replay_mode = REPLAY_OFF;
static FIL replay_file;
static bool replay_file_open = false;
static uint64_t replay_session_id = 0;
static uint32_t replay_frame = 0, replay_total_frames = 0;
static uint8_t replay_input = 0, replay_last_input = 0xff;
static replay_event_t replay_next = {};
static bool replay_next_valid = false;
static uint32_t replay_osd_second = UINT32_MAX;

/* Header code 0x148 specifies ROM size; no volatile filename or flash offset
 * needs to survive a reset for R/P to recognise the same cartridge. */
static uint32_t replay_cart_bytes() {
    const uint8_t code = rom[0x148];
    if (code <= 8) return 32768u << code;
    if (code == 0x52) return 72u * 16384u;
    if (code == 0x53) return 80u * 16384u;
    if (code == 0x54) return 96u * 16384u;
    return 0;
}
static uint32_t replay_cart_tag() {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0x134; i <= 0x14f; ++i) {
        hash ^= rom[i];
        hash *= 16777619u;
    }
    return hash;
}
static void replay_cart_name(char name[24]) {
    unsigned i = 0;
    for (; i < 16; ++i) {
        const uint8_t c = rom[0x134 + i];
        if (!c) break;
        name[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ? (char)c : '_';
    }
    name[i] = '\0';
    if (!i) strcpy(name, "UNTITLED");
}
static void replay_path(char* out, const size_t size) {
    char name[24];
    replay_cart_name(name);
    snprintf(out, size, CONFIG_DIR "/replay/%s-%08lX.gbr", name,
             (unsigned long)replay_cart_tag());
}
static bool replay_checkpoint() {
    if (!replay_file_open || replay_mode != REPLAY_RECORD) return false;
    const FSIZE_t position = f_tell(&replay_file);
    UINT written = 0;
    return f_lseek(&replay_file, offsetof(replay_header_t, total_frames)) == FR_OK &&
           f_write(&replay_file, &replay_frame, sizeof replay_frame, &written) == FR_OK &&
           written == sizeof replay_frame &&
           f_lseek(&replay_file, position) == FR_OK && f_sync(&replay_file) == FR_OK;
}
static void replay_stop() {
    const bool had_recording = replay_mode == REPLAY_RECORD;
    if (replay_file_open) {
        if (had_recording) replay_checkpoint();
        f_close(&replay_file);
    }
    replay_file_open = false;
    replay_mode = REPLAY_OFF;
    replay_next_valid = false;
    replay_osd_second = UINT32_MAX;
#if VGA || HDMI || SOFTTV
    if (had_recording) graphics_set_overlay(nullptr, false);
#endif
}
static void replay_read_next() {
    UINT received = 0;
    replay_next_valid = replay_file_open && replay_mode == REPLAY_PLAY &&
        f_read(&replay_file, &replay_next, sizeof replay_next, &received) == FR_OK &&
        received == sizeof replay_next;
}
static bool replay_start(const bool record) {
    replay_stop();
    if (record) {
        config_mkdirs();
        f_mkdir(CONFIG_DIR "/replay");
    }
    char path[256];
    replay_path(path, sizeof path);
    if (f_open(&replay_file, path, record ? FA_CREATE_ALWAYS | FA_WRITE : FA_READ) != FR_OK)
        return false;
    replay_file_open = true;
    replay_header_t header = {};
    UINT io = 0;
    char name[24] = {};
    replay_cart_name(name);
    if (record) {
        header.magic = REPLAY_MAGIC;
        header.version = 1;
        header.header_size = sizeof header;
        header.rom_bytes = replay_cart_bytes();
        header.rom_tag = replay_cart_tag();
        header.session_id = time_us_64();
        memcpy(header.rom_name, name, sizeof name);
        if (f_write(&replay_file, &header, sizeof header, &io) != FR_OK || io != sizeof header ||
            f_write(&replay_file, ram, sizeof ram, &io) != FR_OK || io != sizeof ram) {
            replay_stop();
            return false;
        }
        replay_mode = REPLAY_RECORD;
        if (f_sync(&replay_file) != FR_OK) { replay_stop(); return false; }
    } else {
        if (f_read(&replay_file, &header, sizeof header, &io) != FR_OK || io != sizeof header ||
            header.magic != REPLAY_MAGIC || header.version != 1 ||
            header.header_size != sizeof header || header.rom_bytes != replay_cart_bytes() ||
            header.rom_tag != replay_cart_tag() ||
            memcmp(header.rom_name, name, sizeof name) != 0 ||
            f_read(&replay_file, ram, sizeof ram, &io) != FR_OK || io != sizeof ram) {
            replay_stop();
            return false;
        }
        replay_mode = REPLAY_PLAY;
    }
    replay_session_id = header.session_id;
    replay_frame = 0;
    replay_total_frames = header.total_frames;
    replay_input = 0;
    replay_last_input = 0xff;
    replay_osd_second = UINT32_MAX;
    if (!record) replay_read_next();
    return true;
}
static uint8_t replay_begin_frame(const uint8_t physical_input) {
    if (replay_mode == REPLAY_RECORD) {
        replay_input = physical_input;
        if (replay_input != replay_last_input) {
            const replay_event_t event = { replay_frame, replay_input };
            UINT written = 0;
            if (f_write(&replay_file, &event, sizeof event, &written) != FR_OK ||
                written != sizeof event) {
                replay_stop();
                return physical_input;
            }
            replay_last_input = replay_input;
        }
    } else if (replay_mode == REPLAY_PLAY) {
        if (replay_frame >= replay_total_frames) {
            replay_stop();
            return physical_input;
        }
        while (replay_next_valid && replay_next.frame == replay_frame) {
            replay_input = replay_next.input;
            replay_read_next();
        }
        if (replay_next_valid && replay_next.frame < replay_frame) {
            replay_stop();
            return physical_input;
        }
    } else return physical_input;
    return replay_input;
}
static void replay_end_frame() {
    if (replay_mode == REPLAY_OFF) return;
    ++replay_frame;
    // Do not seek into the header or force an SD sync from the frame loop.
    // replay_checkpoint() is still called on Save State, rollback and stop.
}
static replay_state_t replay_make_state() {
    replay_state_t state = {};
    if (replay_mode == REPLAY_RECORD && replay_file_open) {
        if (!replay_checkpoint()) { replay_stop(); return state; }
        const FSIZE_t position = f_tell(&replay_file);
        if (position > UINT32_MAX) { replay_stop(); return state; }
        state.magic = REPLAY_STATE_MAGIC;
        state.session_id = replay_session_id;
        state.frame = replay_frame;
        state.file_offset = (uint32_t)position;
        state.input = replay_input;
    }
    return state;
}
static void replay_restore_state(const replay_state_t& state) {
    if (replay_mode == REPLAY_PLAY) { replay_stop(); return; }
    if (replay_mode != REPLAY_RECORD || !replay_file_open) return;
    if (state.magic != REPLAY_STATE_MAGIC || state.session_id != replay_session_id ||
        state.file_offset < sizeof(replay_header_t) + sizeof ram ||
        state.file_offset > f_size(&replay_file) ||
        f_lseek(&replay_file, state.file_offset) != FR_OK ||
        f_truncate(&replay_file) != FR_OK) {
        replay_stop();
        return;
    }
    replay_frame = state.frame;
    replay_input = state.input;
    replay_last_input = 0xff; // write a new first event on the replacement branch
    replay_osd_second = UINT32_MAX;
    if (!replay_checkpoint()) replay_stop();
}
static void replay_draw_record_overlay() {
#if VGA || HDMI || SOFTTV
    if (replay_mode != REPLAY_RECORD) return;
    const uint32_t seconds = replay_frame / 60u;
    if (seconds == replay_osd_second) return;
    replay_osd_second = seconds;
    char label[53];
    snprintf(label, sizeof label, "REC %02lu:%02lu:%02lu",
             (unsigned long)(seconds / 3600u),
             (unsigned long)((seconds / 60u) % 60u),
             (unsigned long)(seconds % 60u));
    graphics_set_overlay(label, true);
#endif
}

static bool save() {
    char pathname[255];
    config_mkdirs();
    state_path(pathname, sizeof pathname, CONFIG_DIR);

    // Flush the movie BEFORE opening the save-state file.  Every state stores
    // the session/frame/file offset required for an exact recording rollback.
    const replay_state_t replay_state = replay_make_state();
    static FIL fd; // static: core 0 has a small stack
    if (FR_OK != f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE))
        return false;
    UINT bw = 0;
    const bool gb_ok = f_write(&fd, &gb, sizeof gb, &bw) == FR_OK && bw == sizeof gb;
    const bool ram_ok = gb_ok && f_write(&fd, ram, sizeof ram, &bw) == FR_OK && bw == sizeof ram;
    const bool replay_ok = ram_ok &&
        f_write(&fd, &replay_state, sizeof replay_state, &bw) == FR_OK && bw == sizeof replay_state;
    const bool closed = f_close(&fd) == FR_OK;
    return replay_ok && closed;
}

static bool load() {
    char pathname[255];
    static FIL fd; // static: core 0 has a small stack
    state_path(pathname, sizeof pathname, CONFIG_DIR);
    if (FR_OK != f_open(&fd, pathname, FA_READ)) {
        state_path(pathname, sizeof pathname, HOME_DIR); // saved by an older version
        if (FR_OK != f_open(&fd, pathname, FA_READ))
            return false;
    }
    // Old save files have just gb + RAM; new ones append the replay marker.
    if (f_size(&fd) < sizeof gb + sizeof ram) { f_close(&fd); return false; }
    UINT br = 0;
    const bool gb_ok = f_read(&fd, &gb, sizeof gb, &br) == FR_OK && br == sizeof gb;
    const bool ram_ok = gb_ok && f_read(&fd, ram, sizeof ram, &br) == FR_OK && br == sizeof ram;
    replay_state_t replay_state = {};
    const bool marker_ok = ram_ok &&
        f_read(&fd, &replay_state, sizeof replay_state, &br) == FR_OK && br == sizeof replay_state;
    f_close(&fd);
    if (!ram_ok) { replay_stop(); return false; }
    if (marker_ok) replay_restore_state(replay_state);
    else if (replay_mode != REPLAY_OFF) replay_stop();
    cgb_palette_reset(); // the loaded GBC palette is not in the video driver yet
    return true;
}
/* TV-SOFT: a TV standard is a coherent set (lines, colour subcarrier,
 * sync), so the menu selects the system; Colors NO sends no colour subcarrier
 * (a black and white picture, as on a black and white TV). */
static uint8_t tv_system = 0; // 0 - PAL, 1 - NTSC
static uint8_t tv_color = 1;
/* VGA / HDMI, 2x and 3x: the gaps between the pixels in grey (as on the LCD)
 * - VGA: 0 - no, 1 - vertical, 2 - horizontal, 3 - both (GRAPHICS_GRAY_*);
 * HDMI: 0 / 2 (horizontal only, the menu shows OFF / ON); grey 0..3 */
static uint8_t gray_lines = 0;
static uint8_t gray_level = 1;
#if HDMI
static uint8_t gray_lines_menu = 0; // OFF / ON
#endif
/* HDMI / VGA / TV-SOFT: picture size (enum graphics_scale_t), by default as
 * the drivers showed it before: HDMI and TV 2x, VGA 3x */
#if VGA
static uint8_t screen_scale = 1;
#else
static uint8_t screen_scale = 0;
#endif
/* HDMI / VGA / TV-SOFT on RP2350: the Game Boy Color picture around the game
 * in 2x and 3x (the cells take 75 KB of RAM - no room on RP2040) */
#if (VGA || HDMI || SOFTTV) && PICO_RP2350
#define BACKPLANE 1
static uint8_t backplane = 1;

/* A backplane file (.gbbp) replaces the built-in one:
 *   "GBBP", version (1 byte, GBBP_VERSION) - other versions are refused;
 *   for HDMI / VGA (320 cells across), then for TV-SOFT (384 cells):
 *     32 colours (R, G, B bytes), the 2x cells, the 3x cells (W x 240 colour
 *     numbers 0..31 each, a row after row; the game window cells are unused).
 * A TV-SOFT build reads its part after the HDMI / VGA one.  Installed from the
 * file manager: copied to /.config/gbc/backplane.gbbp and shown from then on
 * (delete that file for the built-in one). */
#define GBBP_VERSION 1
#define GBBP_FILE CONFIG_DIR "/backplane.gbbp"
static constexpr uint32_t gbbp_part(const uint32_t w) { return BACKPLANE_LOADED_COLORS * 3 + 2 * w * 240; }
static constexpr uint32_t GBBP_HEADER = 5;
static constexpr uint32_t GBBP_SIZE = GBBP_HEADER + gbbp_part(320) + gbbp_part(384);

static bool gbbp_header_ok(FIL* f) {
    uint8_t h[GBBP_HEADER];
    UINT br = 0;
    return f_size(f) == GBBP_SIZE && f_lseek(f, 0) == FR_OK && f_read(f, h, sizeof h, &br) == FR_OK &&
           br == sizeof h && !memcmp(h, "GBBP", 4) && h[4] == GBBP_VERSION;
}

/* the installed backplane, the part of this build, the cells of the scale */
static bool gbbp_load(const enum graphics_scale_t scale) {
    static FIL f; // static: core 0 has a small stack
    if (f_open(&f, GBBP_FILE, FA_READ) != FR_OK) return false;
    bool ok = gbbp_header_ok(&f);
    static uint8_t rgb[BACKPLANE_LOADED_COLORS * 3];
    const uint32_t part = GBBP_HEADER + (BACKPLANE_W == 384 ? gbbp_part(320) : 0);
    const UINT cells_size = BACKPLANE_W * BACKPLANE_H;
    UINT br = 0;
    ok = ok && f_lseek(&f, part) == FR_OK && f_read(&f, rgb, sizeof rgb, &br) == FR_OK && br == sizeof rgb;
    uint8_t* const buffer = ok ? backplane_buffer() : nullptr;
    ok = buffer && f_lseek(&f, part + sizeof rgb + (scale == GRAPHICS_SCALE_3X ? cells_size : 0)) == FR_OK &&
         f_read(&f, buffer, cells_size, &br) == FR_OK && br == cells_size;
    f_close(&f);
    if (ok) backplane_show_loaded(rgb);
    return ok; // otherwise the built-in one is unpacked again
}

static void apply_backplane() {
    const auto scale = (enum graphics_scale_t)screen_scale;
    if (backplane && scale != GRAPHICS_SCALE_STRETCH && gbbp_load(scale)) return;
    graphics_set_backplane(backplane, scale);
}

static bool gbbp_try_install(const char* pathname) {
    const char* dot = strrchr(pathname, '.');
    if (!dot || strcasecmp(dot, ".gbbp") != 0) return false;

    const int window_w = (int)TEXTMODE_COLS < 43 ? (int)TEXTMODE_COLS : 43;
    const int window_y = (TEXTMODE_ROWS - 5) / 2;
    const int window_x = ((int)TEXTMODE_COLS - window_w) / 2;
    draw_window("Backplan", window_x, window_y, window_w, 5);

    static FIL src, dst; // static: core 0 has a small stack
    const char* message = "This backplan version is unsupported";
    bool installed = false;
    if (f_open(&src, pathname, FA_READ) == FR_OK) {
        if (gbbp_header_ok(&src)) {
            message = "ERROR: can't write the backplan";
            config_mkdirs();
            if (f_lseek(&src, 0) == FR_OK && f_open(&dst, GBBP_FILE, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                static uint8_t chunk[4096];
                bool ok = true;
                for (UINT br = 1; ok && br;) {
                    UINT bw = 0;
                    ok = f_read(&src, chunk, sizeof chunk, &br) == FR_OK &&
                         f_write(&dst, chunk, br, &bw) == FR_OK && bw == br;
                }
                ok = f_close(&dst) == FR_OK && ok;
                if (ok) {
                    message = "Backplan is installed";
                    installed = true;
                }
                else {
                    f_unlink(GBBP_FILE);
                }
            }
        }
        f_close(&src);
    }
    if (installed) apply_backplane();
    draw_text(message, window_x + 1, window_y + 2, installed ? 10 : 13, 1);
    sleep_ms(2000);
    return true;
}
#else
static bool gbbp_try_install(const char*) { return false; }
#endif
/* the video settings of the menu to the driver */
static void apply_video_settings() {
#if SOFTTV
    graphics_set_tv(tv_system == 0, tv_color != 0); // applied by the next graphics_set_mode()
#endif
#if VGA || HDMI || SOFTTV
    graphics_set_scale((enum graphics_scale_t)screen_scale);
#endif
#if VGA || HDMI
    graphics_set_gray_lines(gray_lines, gray_level);
#endif
#if BACKPLANE
    apply_backplane();
#endif
}

#ifdef TEXTMODE_COLS_MAX // text columns change at run time (TV-SOFT)
#define TEXT_BUFFER_COLS TEXTMODE_COLS_MAX
#else
#define TEXT_BUFFER_COLS TEXTMODE_COLS
#endif

static const uint32_t palette_color_pool[][4] = {
    // B: background, C: sprite 0, D: sprite 1, E: border.
    // Warm: Red + Orange (6 entries)
    {0xFFF1F1, 0xFF6A6A, 0xB71919, 0x750909},
    {0xFFD6D6, 0xFF5353, 0x950E0E, 0x560202},
    {0xFFBEBE, 0xEF3C3C, 0x7F0404, 0x3F0101},
    {0xFFFBF1, 0xDBA30F, 0x926A00, 0x0C0422},
    {0xFDEFCA, 0xC69209, 0x846208, 0x5B4202},
    {0xFFD871, 0xB58504, 0x755707, 0x443100},
    // Cold: Green + Blue + Purple (9 entries)
    {0xF1FFF8, 0x41E291, 0x1C9E5C, 0x0E6439},
    {0xC3FFE1, 0x2ACD7B, 0x159051, 0x085B32},
    {0x86F8BF, 0x27BB70, 0x15824C, 0x04361D},
    {0xF1FCFF, 0x72DCF6, 0x2AA1C0, 0x127088},
    {0xCFF5FF, 0x56CAE7, 0x2493B0, 0x0A5062},
    {0xB5F0FF, 0x44BDDB, 0x1E8AA6, 0x03232C},
    {0xEBEEF4, 0x89A1CC, 0x2A5EBC, 0x0D388B},
    {0xD3D9E5, 0x6B8DCA, 0x2355B0, 0x042460},
    {0xC9D2E3, 0x4C79C9, 0x1B489C, 0x011334},
};
static constexpr uint8_t PALETTE_WARM_COUNT = 6;

static uint32_t rgb565_to_rgb888(const uint16_t c) {
    const uint32_t r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

/* the colours in use go to the video driver (slots 0..11, their mixes for
 * the LCD ghosting 12..143); a Game Boy Color game keeps its own palette in
 * slots 0..191 */
static void palette_apply() {
    if (gb.cgb.cgbMode) return;
    uint32_t rgb[DMG_SLOTS];
    for (int i = 0; i < LAYER_COUNT; i++)
        for (int j = 0; j < 4; j++) {
            const uint32_t c = pal_rgb[i][j];
            // RGB888() does not bracket its arguments: plain values only
            const uint32_t r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
            const uint8_t slot = layer_hw[i] * 4 + j;
            graphics_set_palette(slot, RGB888(r, g, b));
            palette[layer_hw[i]][j] = slot;
            rgb[slot] = c;
        }
    for (uint8_t a = 0; a < DMG_SLOTS; a++)
        for (uint8_t b = 0; b < DMG_SLOTS; b++) {
            if (a == b) continue;
            const uint32_t r = ((rgb[a] >> 16 & 0xFF) + (rgb[b] >> 16 & 0xFF) + 1) / 2;
            const uint32_t g = ((rgb[a] >> 8 & 0xFF) + (rgb[b] >> 8 & 0xFF) + 1) / 2;
            const uint32_t bl = ((rgb[a] & 0xFF) + (rgb[b] & 0xFF) + 1) / 2;
            graphics_set_palette(blend_slot[a][b], RGB888(r, g, bl));
        }
}

/* The sixteen shades are indexed as current DMG shade * 4 + old shade.
 * The current shade chooses the colour family; the previous shade chooses
 * the transient shade.  Each layer uses the same selected 16-entry preset.
 * This only replaces DMG palette slots, never the GBC 0..191 slots.
 * The standard palette_apply() remains intact for all existing presets.
 */
static void palette16_apply() {
    if (gb.cgb.cgbMode || palette16_select == 0 || palette16_select > PALETTE16_CUSTOM)
        return;
    const uint32_t* const rgb = palette16_select == PALETTE16_CUSTOM ?
        custom_shades16 : gb_palettes16[palette16_select - 1];
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        for (unsigned shade = 0; shade < 4; shade++) {
            const uint32_t c = rgb[shade * 4 + shade];
            const uint32_t r = (c >> 16) & 255u, g = (c >> 8) & 255u, b = c & 255u;
            graphics_set_palette(layer_hw[layer] * 4 + shade, RGB888(r, g, b));
        }
    /* LCD ghosting: the transition from the previous shade to the current one
     * is the table entry itself, current * 4 + previous.  Every ordered
     * (previous, current) pair has its own slot (blend_slots_init). */
    for (unsigned prev = 0; prev < DMG_SLOTS; prev++)
        for (unsigned curr = 0; curr < DMG_SLOTS; curr++) {
            if (prev == curr) continue;
            const uint32_t c = rgb[(curr & 3u) * 4 + (prev & 3u)];
            const uint32_t r = (c >> 16) & 255u, g = (c >> 8) & 255u, b = c & 255u;
            graphics_set_palette(blend_slot[prev][curr], RGB888(r, g, b));
        }
}

static uint32_t palette_random_state = 0;

static uint32_t palette_random_next() {
    if (!palette_random_state)
        palette_random_state = (uint32_t)time_us_64() ^ 0x9E3779B9u;
    palette_random_state ^= palette_random_state << 13;
    palette_random_state ^= palette_random_state >> 17;
    palette_random_state ^= palette_random_state << 5;
    return palette_random_state;
}

/* Select a colour from the opposite-temperature part of the spreadsheet. */
static unsigned palette_opposite_index(const unsigned index) {
    if (index < PALETTE_WARM_COUNT)
        return PALETTE_WARM_COUNT +
               palette_random_next() % (count_of(palette_color_pool) - PALETTE_WARM_COUNT);
    return palette_random_next() % PALETTE_WARM_COUNT;
}

/* A 4-colour DMG palette: background and border have opposite temperatures;
 * OBJ0 and OBJ1 also have opposite temperatures, independently of BG. */
static void randomize_layer(const unsigned layer) {
    uint32_t* const c = random_rgb[layer];
    const unsigned background = palette_random_next() % count_of(palette_color_pool);
    const unsigned sprite0 = palette_random_next() % count_of(palette_color_pool);
    c[0] = palette_color_pool[background][0];
    c[1] = palette_color_pool[sprite0][1];
    c[2] = palette_color_pool[palette_opposite_index(sprite0)][2];
    c[3] = palette_color_pool[palette_opposite_index(background)][3];
}

/* the selector a palette follows */
static uint8_t layer_selector(const unsigned layer) {
    return palette_all == PALETTE_CUSTOM ? palette_index[layer] : palette_all;
}

/* the palettes get their own selectors (keeping the colours shown) */
static void palette_split() {
    if (palette_all == PALETTE_CUSTOM) return;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) palette_index[layer] = palette_all;
    palette_all = PALETTE_CUSTOM;
}

static void randomize_random_layers() {
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        if (layer_selector(layer) == PALETTE_RANDOM) randomize_layer(layer);
}

/* the colours of every palette by its selector */
static void update_palette() {
    bool gbc_palette_ready = false;
    uint8_t gbc_palette_of = 0xff;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) {
        const uint8_t sel = layer_selector(layer);
        uint32_t* const out = pal_rgb[layer];
        if (sel < PALETTE_MONO_FIRST) {
            // AUTO / a GBC boot palette: the colours this palette gets there
            if (!gbc_palette_ready || gbc_palette_of != sel) {
                if (sel == PALETTE_AUTO) {
                    char rom_title[16];
                    auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
                }
                else {
                    manual_assign_palette(palette16, sel);
                }
                gbc_palette_ready = true;
                gbc_palette_of = sel;
            }
            for (int j = 0; j < 4; j++) out[j] = rgb565_to_rgb888(palette16[layer_hw[layer]][j]);
        }
        else if (sel < PALETTE_RANDOM) {
            const uint8_t* m = mono_palettes[sel - PALETTE_MONO_FIRST];
            for (int j = 0; j < 4; j++) out[j] = m[j * 3] << 16 | m[j * 3 + 1] << 8 | m[j * 3 + 2];
        }
        else if (sel == PALETTE_RANDOM) {
            memcpy(out, random_rgb[layer], sizeof random_rgb[layer]);
        }
        else {
            memcpy(out, custom_rgb[layer], sizeof custom_rgb[layer]);
        }
    }
    palette_apply();
    palette16_apply();
}

/* the colours saved for a game: /.config/gbc/<ROM file name>.ini */
static bool game_palette_ini_path(char* pathname, const size_t size) {
    if (!rom_file[0]) return false;
    char basename[80];
    snprintf(basename, sizeof basename, "%s", rom_file);
    char* dot = strrchr(basename, '.');
    if (dot && dot != basename) *dot = '\0';
    return snprintf(pathname, size, CONFIG_DIR "/%s.ini", basename) > 0;
}

static bool game_palette_ini_exists() {
    char pathname[128];
    FILINFO info;
    return game_palette_ini_path(pathname, sizeof pathname) && FR_OK == f_stat(pathname, &info);
}

static const char* const ini_layer_keys[LAYER_COUNT] = { "bg=", "obj0=", "obj1=" };

/* [palette]: the Custom colours, [layers]: the selectors; a Random palette is
 * kept with the colours it drew (as Custom) */
static bool write_game_palette_ini() {
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;
    config_mkdirs();

    static char data[256];
    int len = snprintf(data, sizeof data, "[palette]\r\n");
    for (unsigned i = 0; i < LAYER_COUNT && len > 0 && len < (int)sizeof data; i++) {
        const uint32_t* c = layer_selector(i) == PALETTE_RANDOM ? pal_rgb[i] : custom_rgb[i];
        len += snprintf(data + len, sizeof data - len, "%s%06lX,%06lX,%06lX,%06lX\r\n", ini_layer_keys[i],
                        (unsigned long)c[0], (unsigned long)c[1], (unsigned long)c[2], (unsigned long)c[3]);
    }
    const bool all = palette_all != PALETTE_CUSTOM && palette_all != PALETTE_RANDOM;
    if (len > 0 && len < (int)sizeof data)
        len += snprintf(data + len, sizeof data - len, "[layers]\r\nall=%u\r\n",
                        all ? (unsigned)palette_all : (unsigned)PALETTE_CUSTOM);
    for (unsigned i = 0; i < LAYER_COUNT && len > 0 && len < (int)sizeof data; i++) {
        const unsigned sel = all ? palette_index[i]
                           : layer_selector(i) == PALETTE_RANDOM ? PALETTE_CUSTOM : layer_selector(i);
        len += snprintf(data + len, sizeof data - len, "%s%u\r\n", ini_layer_keys[i], sel);
    }
    if (len > 0 && len < (int)sizeof data)
        len += snprintf(data + len, sizeof data - len, "palette16=%u\r\npalette_list=%u\r\n",
                        (unsigned)palette16_select, PALETTE_LIST_VERSION);
    if (len <= 0 || len >= (int)sizeof data) return false;

    static FIL file; // static: core 0 has a small stack
    if (FR_OK != f_open(&file, pathname, FA_CREATE_ALWAYS | FA_WRITE)) return false;
    UINT written = 0;
    const FRESULT fr = f_write(&file, data, (UINT)len, &written);
    const FRESULT close_fr = f_close(&file);
    return fr == FR_OK && written == (UINT)len && close_fr == FR_OK;
}

static bool parse_rgb4(const char* text, uint32_t rgb[4]) {
    unsigned long c[4];
    if (!text || 4 != sscanf(text, "%lx,%lx,%lx,%lx", &c[0], &c[1], &c[2], &c[3])) return false;
    for (int j = 0; j < 4; j++) {
        if (c[j] > 0xFFFFFFul) return false;
        rgb[j] = (uint32_t)c[j];
    }
    return true;
}

static bool load_game_palette_ini() {
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;

    static FIL file; // static: core 0 has a small stack
    if (FR_OK != f_open(&file, pathname, FA_READ)) return false;
    static char data[320];
    UINT bytes_read = 0;
    const FRESULT fr = f_read(&file, data, sizeof data - 1, &bytes_read);
    f_close(&file);
    if (fr != FR_OK || bytes_read == 0) return false;
    data[bytes_read] = '\0';

    uint32_t rgb[LAYER_COUNT][4];
    char* const layers = strstr(data, "[layers]");
    if (layers) *layers = '\0'; // the colours are searched in [palette] only
    for (unsigned i = 0; i < LAYER_COUNT; i++) {
        const char* line = strstr(data, ini_layer_keys[i]);
        if (!line || !parse_rgb4(line + strlen(ini_layer_keys[i]), rgb[i])) return false;
    }
    memcpy(custom_rgb, rgb, sizeof custom_rgb);
    for (unsigned i = 0; i < LAYER_COUNT; i++) {
        // no [layers] (a file of the previous version): every palette is Custom
        const char* line = layers ? strstr(layers + 1, ini_layer_keys[i]) : nullptr;
        const unsigned long sel = line ? strtoul(line + strlen(ini_layer_keys[i]), nullptr, 10) : PALETTE_CUSTOM;
        palette_index[i] = sel <= PALETTE_CUSTOM ? (uint8_t)sel : PALETTE_CUSTOM;
    }
    // no "all=" (an older file): every palette by its own selector
    const char* all = layers ? strstr(layers + 1, "all=") : nullptr;
    const unsigned long sel_all = all ? strtoul(all + 4, nullptr, 10) : PALETTE_CUSTOM;
    palette_all = sel_all <= PALETTE_CUSTOM ? (uint8_t)sel_all : PALETTE_CUSTOM;
    if (!(layers && strstr(layers + 1, "palette_list="))) { // the numbers of list 1
        palette_all = palette_from_list1(palette_all);
        for (unsigned i = 0; i < LAYER_COUNT; i++) palette_index[i] = palette_from_list1(palette_index[i]);
    }
    const char* p16 = layers ? strstr(layers + 1, "palette16=") : nullptr;
    const unsigned long sel16 = p16 ? strtoul(p16 + sizeof("palette16=") - 1, nullptr, 10) : 0;
    palette16_select = sel16 <= PALETTE16_CUSTOM ? (uint8_t)sel16 : 0;
    randomize_random_layers();
    update_palette();
    return true;
}

static bool game_ini_action() {
    if (gb.cgb.cgbMode) return false;
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;
    if (game_ini_linked) {
        const FRESULT fr = f_unlink(pathname);
        if (fr == FR_OK || fr == FR_NO_FILE) game_ini_linked = false;
    }
    else if (write_game_palette_ini()) {
        game_ini_linked = true;
    }
    return false;
}

static void apply_game_palette_on_load() {
    palette16_select = palette16_global;
    game_ini_linked = game_palette_ini_exists();
    if (!load_game_palette_ini()) {
        randomize_random_layers(); // new colours for every game
        update_palette();
    }
}

static void f_save_conf(void);

/* ---- the palette editor: the 12 colours as 3 rows (BG, OBJ0, OBJ1) of 4 ---- */
static constexpr int PE_W = LCD_WIDTH / 4;  // 40
static constexpr int PE_H = LCD_HEIGHT / 3; // 48

/* 3x5 glyphs: 0-9, A-F, G, J, O, space */
static void pe_char(uint8_t* buf, const int x, const int y, const char ch, const uint8_t color) {
    static const char chars[] = "0123456789ABCDEFGJO ";
    static const uint8_t glyphs[][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
        {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
        {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {7, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
        {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7}, {7, 4, 7, 4, 4},
        {7, 4, 5, 5, 7}, {1, 1, 1, 5, 7}, {2, 5, 5, 5, 2}, {0, 0, 0, 0, 0},
    };
    const char* p = strchr(chars, ch);
    if (!p || !ch) return;
    const uint8_t* g = glyphs[p - chars];
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (g[row] & (4u >> col))
                buf[(y + row) * LCD_WIDTH + x + col] = color;
}

static void pe_text(uint8_t* buf, int x, const int y, const char* s, const uint8_t color) {
    for (; *s; s++, x += 4) pe_char(buf, x, y, *s, color);
}

/* the most distant of the 12 colours keeps the text readable */
static uint8_t pe_contrast(const unsigned slot) {
    const uint32_t bg = pal_rgb[slot >> 2][slot & 3];
    uint8_t best = 0;
    unsigned best_distance = 0;
    for (unsigned i = 0; i < LAYER_COUNT * 4; i++) {
        const uint32_t fg = pal_rgb[i >> 2][i & 3];
        const int dr = (int)(bg >> 16 & 0xFF) - (int)(fg >> 16 & 0xFF);
        const int dg = (int)(bg >> 8 & 0xFF) - (int)(fg >> 8 & 0xFF);
        const int db = (int)(bg & 0xFF) - (int)(fg & 0xFF);
        const unsigned distance = dr * dr + dg * dg + db * db;
        if (distance > best_distance) {
            best_distance = distance;
            best = layer_hw[i >> 2] * 4 + (i & 3); // the video palette slot
        }
    }
    return best;
}

static void pe_draw(uint8_t* buf, const unsigned selected, const int edit_channel) {
    static const char* const names[LAYER_COUNT] = { "BG", "OBJ0", "OBJ1" };
    for (unsigned slot = 0; slot < LAYER_COUNT * 4; slot++) {
        const int x0 = (int)(slot & 3) * PE_W;
        const int y0 = (int)(slot >> 2) * PE_H;
        for (int y = y0; y < y0 + PE_H; y++)
            memset(buf + y * LCD_WIDTH + x0, layer_hw[slot >> 2] * 4 + (slot & 3), PE_W);

        const uint8_t text = pe_contrast(slot);
        char label[8];
        snprintf(label, sizeof label, "%s %u", names[slot >> 2], slot & 3);
        pe_text(buf, x0 + (PE_W - (int)strlen(label) * 4 + 1) / 2, y0 + 15, label, text);
        char rgb[8];
        snprintf(rgb, sizeof rgb, "%06lX", (unsigned long)pal_rgb[slot >> 2][slot & 3]);
        pe_text(buf, x0 + 8, y0 + 26, rgb, text);

        if (slot == selected) { // the focus frame
            for (int x = x0 + 1; x < x0 + PE_W - 1; x++) {
                buf[(y0 + 1) * LCD_WIDTH + x] = text;
                buf[(y0 + PE_H - 2) * LCD_WIDTH + x] = text;
            }
            for (int y = y0 + 1; y < y0 + PE_H - 1; y++) {
                buf[y * LCD_WIDTH + x0 + 1] = text;
                buf[y * LCD_WIDTH + x0 + PE_W - 2] = text;
            }
            if (edit_channel >= 0) { // the edited byte: RR, GG or BB
                const int dx = x0 + 8 + edit_channel * 8;
                for (int x = dx; x < dx + 7; x++)
                    buf[(y0 + 32) * LCD_WIDTH + x] = text;
            }
        }
    }
}

typedef struct {
    bool active;
    uint64_t next_repeat;
} pe_repeat_t;

static bool pe_repeat(const bool level, pe_repeat_t* state) {
    const uint64_t now = time_us_64();
    if (!level) {
        state->active = false;
        return false;
    }
    if (!state->active) {
        state->active = true;
        state->next_repeat = now + 300000; // initial key-repeat delay
        return true;
    }
    if (now >= state->next_repeat) {
        state->next_repeat = now + 80000;
        return true;
    }
    return false;
}

/* The first edit of a colour turns its palette into Custom with the colours
 * shown, the other two palettes keep their colours (with their own selectors). */
static void pe_ensure_custom(const unsigned layer) {
    if (layer_selector(layer) == PALETTE_CUSTOM) return;
    palette_split();
    memcpy(custom_rgb[layer], pal_rgb[layer], sizeof custom_rgb[layer]);
    palette_index[layer] = PALETTE_CUSTOM;
}

/* Arrows - a colour, Enter / A - edit it (left / right: R, G, B; up / down:
 * the value), hex digits type RRGGBB, Esc / B - back, gamepad START or F12 -
 * close.  The picture is drawn in the back buffer (the game is paused). */
static void palette_editor(const bool from_menu) {
    if (gb.cgb.cgbMode) return; // a Game Boy Color game has its own colours
    for (int i = 0; i < 100 && frame_pending >= 0; i++) sleep_ms(1); // the render core takes it
    uint8_t* const buf = (uint8_t *)frames[frame_back];
    unsigned selected = 0;
    int edit_channel = -1;
    int hex_digit = -1;

    kbd_hex_key = -1;
    kbd_hex_mode = true;
    palette_editor_requested = false;
    if (from_menu) graphics_set_mode(GRAPHICSMODE_DEFAULT);
    graphics_set_buffer(buf, LCD_WIDTH, LCD_HEIGHT);
    palette_apply();
    pe_draw(buf, selected, edit_channel);

    auto accept_level = [] { return keyboard.start || keyboard.a || (nespad_state & DPAD_A); };
    auto back_level = [] { return keyboard.select || keyboard.b || (nespad_state & DPAD_B); };
    auto close_level = [] { return (nespad_state & DPAD_START) != 0; };
    bool accept_prev = accept_level(), back_prev = back_level(), close_prev = close_level();
    pe_repeat_t rep_left = {}, rep_right = {}, rep_up = {}, rep_down = {};

    while (true) {
        const bool accept_now = accept_level(), back_now = back_level(), close_now = close_level();
        const bool accept = accept_now && !accept_prev;
        const bool back = back_now && !back_prev;
        const bool close = (close_now && !close_prev) || palette_editor_requested;
        accept_prev = accept_now;
        back_prev = back_now;
        close_prev = close_now;
        palette_editor_requested = false;
        const bool left = pe_repeat(gamepad_bits.left, &rep_left);
        const bool right = pe_repeat(gamepad_bits.right, &rep_right);
        const bool up = pe_repeat(gamepad_bits.up, &rep_up);
        const bool down = pe_repeat(gamepad_bits.down, &rep_down);
        const unsigned layer = selected >> 2;
        uint32_t* const value = &pal_rgb[layer][selected & 3];
        bool redraw = false, changed = false;

        const int8_t typed = kbd_hex_key;
        kbd_hex_key = -1;
        if (close) {
            break;
        }
        if (typed >= 0) {
            if (hex_digit < 0) hex_digit = 0;
            const unsigned shift = (5 - hex_digit) * 4u;
            pe_ensure_custom(layer);
            *value = (*value & ~(0xFu << shift)) | (uint32_t)typed << shift;
            changed = true;
            if (++hex_digit >= 6) {
                hex_digit = -1;
                edit_channel = -1;
            }
            else {
                edit_channel = hex_digit >> 1;
            }
        }
        else if (edit_channel < 0) {
            if (left) selected = (selected & ~3u) | ((selected - 1) & 3u);
            else if (right) selected = (selected & ~3u) | ((selected + 1) & 3u);
            else if (up) selected = (selected + LAYER_COUNT * 4 - 4) % (LAYER_COUNT * 4);
            else if (down) selected = (selected + 4) % (LAYER_COUNT * 4);
            else if (accept) edit_channel = 0;
            else if (back) break;
            redraw = left || right || up || down || accept;
        }
        else {
            if (left) edit_channel = (edit_channel + 2) % 3;
            else if (right) edit_channel = (edit_channel + 1) % 3;
            else if (up || down) {
                const unsigned shift = (2 - edit_channel) * 8u;
                unsigned v = *value >> shift & 0xFF;
                if (up && v < 255) v++;
                if (down && v > 0) v--;
                pe_ensure_custom(layer);
                *value = (*value & ~(0xFFu << shift)) | v << shift;
                changed = true;
            }
            else if (accept || back) {
                edit_channel = -1;
                hex_digit = -1;
            }
            redraw = true;
        }
        if (changed) {
            custom_rgb[layer][selected & 3] = *value;
            palette_apply();
            redraw = true;
        }
        if (redraw) pe_draw(buf, selected, edit_channel);
        sleep_ms(10);
    }

    kbd_hex_mode = false;
    kbd_hex_key = -1;
    f_save_conf();
    graphics_set_buffer((uint8_t *)frames[frame_front], LCD_WIDTH, LCD_HEIGHT);
    if (from_menu) graphics_set_mode(TEXTMODE_DEFAULT);
}

/* keys pressed in the ROM browser or in the menu are not for the game */
static void palette_hotkeys_clear() {
    palette_layer_random_requested = 0;
    palette_shuffle_requested = palette_shuffle_requested_no_save = false;
    palette_save_requested = palette_default_requested = palette_editor_requested = false;
}

/* F1..F3, F6, F7, F8, F9, F12 - see process_kbd_report();
 * returns true after the palette editor (keys may still be held) */
static bool handle_palette_hotkeys() {
    const uint8_t random_layer = palette_layer_random_requested;
    const bool shuffle = palette_shuffle_requested;
    const bool shuffle_no_save = palette_shuffle_requested_no_save;
    const bool save_game = palette_save_requested;
    const bool to_default = palette_default_requested;
    const bool editor = palette_editor_requested;
    palette_layer_random_requested = 0;
    palette_shuffle_requested = palette_shuffle_requested_no_save = false;
    palette_save_requested = palette_default_requested = palette_editor_requested = false;
    if (gb.cgb.cgbMode) return false; // a Game Boy Color game has its own colours
    /* The randomizer and normal palette editor return to the 4-shade mode.
     * F8 alone does not: it binds the currently selected 16-shade mode. */
    if (random_layer || shuffle || shuffle_no_save || to_default || editor)
        palette16_select = 0;

    if (random_layer > 0 && random_layer <= LAYER_COUNT) {
        const unsigned layer = random_layer - 1;
        palette_split();
        palette_index[layer] = PALETTE_RANDOM;
        randomize_layer(layer);
        update_palette();
    }
    if (shuffle || shuffle_no_save) {
        palette_all = PALETTE_RANDOM;
        randomize_random_layers();
        update_palette();
    }
    if ((shuffle || save_game) && write_game_palette_ini())
        game_ini_linked = true;
    if (to_default) {
        palette_all = PALETTE_AUTO;
        update_palette();
        f_save_conf();
    }
    if (editor) {
        palette_editor(false);
        return true;
    }
    return false;
}

/* the names of the palette selectors, in the PALETTE_* order */
#define PALETTE_NAMES \
    "AUTO (by game)   ", \
    "YELLOW-RED       ", \
    "ORANGE           ", \
    "NEGATIVE         ", \
    "DARK GREEN       ", \
    "RED              ", \
    "PINK             ", \
    "GREEN            ", \
    "DARK BLUE        ", \
    "PASTEL           ", \
    "BLUE             ", \
    "YELLOW           ", \
    "DMG              ", \
    "AMERICANA        ", \
    "AUTUMN CHILL     ", \
    "B4SEMENT         ", \
    "BALDUR NEON DARK ", \
    "BICYCLE          ", \
    "CARAMEL AUTUMN   ", \
    "CHERRYMELON      ", \
    "CORAL 4          ", \
    "COSMIC CREAM GB  ", \
    "DREAM CANDY      ", \
    "DUSTBYTE         ", \
    "DYING CONTRAST   ", \
    "FORGOTTEN SWAMP  ", \
    "FUZZYFOUR        ", \
    "H3RRY FZ         ", \
    "ITALY 4          ", \
    "LASER LAB        ", \
    "MANGAVANIA       ", \
    "PONG4            ", \
    "PURPLEDAWN       ", \
    "RED POSTER       ", \
    "RETRO VIBES      ", \
    "RETTIEPUNK       ", \
    "RISING SUN GB    ", \
    "SLURRY GB        ", \
    "SODA CAP         ", \
    "SOFTSERVE 4      ", \
    "STAR POP         ", \
    "T LOLLIPOP       ", \
    "VOLTAGE WARNING  ", \
    "GAMATE DEFAULT   ", \
    "BLACK & WHITE    ", \
    "BGB              ", \
    "AUTUMN FOREST    ", \
    "OCEAN SAND       ", \
    "MINT SAND        ", \
    "AMBER            ", \
    "GREEN            ", \
    "BLUE             ", \
    "WATAROO          ", \
    "GB_DMG           ", \
    "GB_POCKET        ", \
    "GB_LIGHT         ", \
    "BLOSSOM_PINK     ", \
    "BUBBLES_BLUE     ", \
    "BUTTERCUP_GREEN  ", \
    "DIGIVICE         ", \
    "GAME_COM         ", \
    "GAMEKING         ", \
    "GAME_MASTER      ", \
    "GOLDEN_WILD      ", \
    "GREENSCALE       ", \
    "HOKAGE_ORANGE    ", \
    "LABO_FAWN        ", \
    "SUPER_SAIYAN     ", \
    "MICROVISION      ", \
    "MILLION_LIVE_GOLD", \
    "ODYSSEY_GOLD     ", \
    "SHINY_SKY_BLUE   ", \
    "SLIME_BLUE       ", \
    "TI_83            ", \
    "TRAVEL_WOOD      ", \
    "VIRTUAL_BOY      ", \
    "TV-LINK          ", \
    "RANDOM           ", \
    "CUSTOM           "

const MenuItem menu_items[] = {
    //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    { "Swap AB <> BA: %s", ARRAY, &swap_ab,  nullptr, 1, {"NO ", "YES"}},
    { "Palette:      %s", ARRAY, &palette_all, nullptr, PALETTE_CUSTOM,
        { PALETTE_NAMES }
    },
    { "  BG:         %s", ARRAY, &palette_index[LAYER_BG], nullptr, PALETTE_CUSTOM,
        { PALETTE_NAMES }
    },
    { "  OBJ0:       %s", ARRAY, &palette_index[LAYER_OBJ0], nullptr, PALETTE_CUSTOM,
        { PALETTE_NAMES }
    },
    { "  OBJ1:       %s", ARRAY, &palette_index[LAYER_OBJ1], nullptr, PALETTE_CUSTOM,
        { PALETTE_NAMES }
    },
    { "16 shades: %s", ARRAY, &palette16_select, nullptr, PALETTE16_CUSTOM,
      { "OFF    ", "GRAY   ", "RED    ", "ORANGE ", "GREEN  ", "BLUE   ", "PURPLE ",
        "OGBP   ", "OGBR   ", "GBPR   ", "GBPO   ", "BPRO   ", "BROG   ", "PROG   ",
        "POGR   ", "ROGB   ", "ROGP   ", "CUSTOM " } },
    { "Palette editor", PALETTE_EDITOR },
    { "", GAME_INI, nullptr, &game_ini_action },
    { "LCD ghosting: %s", ARRAY, &lcd_ghosting, nullptr, 1, { "OFF", "ON " } },
    {},
    { "Demo time: %s", ARRAY, &demo_duration, nullptr, 7,
      { "15 sec", "30 sec", "45 sec", "1 min ", "2 min ", "3 min ", "5 min ", "10 min" } },
    { "Demo name OSD: %s", ARRAY, &demo_name, nullptr, 1, { "OFF", "ON " } },
    { "Demo timer OSD: %s", ARRAY, &demo_timer_osd, nullptr, 1, { "OFF", "ON " } },
    { "Save state: %i", INT, &save_slot, &save, 8 },
    { "Load state: %i", INT, &save_slot, &load, 8 },
#if SOFTTV
    { "" },
    { "TV system %s", ARRAY, &tv_system, nullptr, 1, { "PAL ", "NTSC" } },
    { "Colors: %s", ARRAY, &tv_color, nullptr, 1, { "NO ", "YES" } },
#endif
#if VGA || HDMI || SOFTTV
    { "" },
    { "Screen: %s", ARRAY, &screen_scale, nullptr, 2, { "2x  ", "3x  ", "Full" } },
#if BACKPLANE
    { "Backplane: %s", ARRAY, &backplane, nullptr, 1, { "OFF", "ON " } },
#endif
#endif
#if VGA
    { "Gray lines: %s", ARRAY, &gray_lines, nullptr, 3, { "No        ", "Vertical  ", "Horizontal", "Both      " } },
#elif HDMI
    { "Gray lines: %s", ARRAY, &gray_lines_menu, nullptr, 1, { "OFF", "ON " } },
#endif
#if VGA || HDMI
    { "Gray level: %s", ARRAY, &gray_level, nullptr, 3, { "0", "1", "2", "3" } },
#endif
    {},
{
    "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
    { FREQ_STR(CPU_FREQ), "396", "404", "408", "412", "416", "420", "424", "432" }
},
{ "Press START / Enter to apply", NONE },
    { "Start Demo", START_DEMO },
    { "Reset to ROM select", ROM_SELECT },
    { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

/* key=value text: a new key never invalidates an existing file */
static void config_path(char* pathname, const size_t size) {
    snprintf(pathname, size, CONFIG_DIR "/%s/%s/config.ini", config_platform_name(), config_video_name());
}

static void f_load_conf(void) {
    static FIL f; // static: core 0 has a small stack
    char pathname[96];
    config_path(pathname, sizeof pathname);
    if (f_open(&f, pathname, FA_READ) == FR_OK) {
        static char text[768];
        UINT br = 0;
        if (f_read(&f, text, sizeof text - 1, &br) != FR_OK) br = 0;
        f_close(&f);
        text[br] = '\0';
        unsigned long palette_list = 1;
        for (char* line = strtok(text, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
            char* val = strchr(line, '=');
            if (!val) continue;
            *val++ = '\0';
            const unsigned long d = strtoul(val, nullptr, 10);
            if (!strcmp(line, "swap_ab")) swap_ab = d ? 1 : 0;
            else if (!strcmp(line, "lcd_ghosting")) lcd_ghosting = d ? 1 : 0;
            else if (!strcmp(line, "demo_duration")) demo_duration = d < count_of(demo_seconds) ? (uint8_t)d : 0;
            else if (!strcmp(line, "demo_name")) demo_name = d ? 1 : 0;
            else if (!strcmp(line, "demo_timer_osd")) demo_timer_osd = d ? 1 : 0;
            else if (!strcmp(line, "tv_system")) tv_system = d <= 1 ? (uint8_t)d : 0;
            else if (!strcmp(line, "tv_color")) tv_color = d ? 1 : 0;
            else if (!strcmp(line, "gray_lines")) gray_lines = d <= 3 ? (uint8_t)d : 0;
            else if (!strcmp(line, "gray_level")) gray_level = d <= 3 ? (uint8_t)d : 1;
#if BACKPLANE
            else if (!strcmp(line, "backplane")) backplane = d ? 1 : 0;
#endif
            else if (!strcmp(line, "palette_all")) palette_all = d <= PALETTE_CUSTOM ? (uint8_t)d : PALETTE_AUTO;
            else if (!strcmp(line, "palette_list")) palette_list = d;
            else if (!strcmp(line, "palette16"))
                palette16_global = palette16_select = d <= PALETTE16_CUSTOM ? (uint8_t)d : 0;
            else if (!strcmp(line, "custom16")) {
                uint32_t rgb[16];
                const char* p = val;
                bool valid = true;
                for (unsigned i = 0; i < 16; i++) {
                    char* end;
                    const unsigned long c = strtoul(p, &end, 16);
                    if (end - p != 6 || c > 0xFFFFFFul ||
                        (i < 15 ? *end != ',' : *end != '\0')) {
                        valid = false;
                        break;
                    }
                    rgb[i] = (uint32_t)c;
                    p = end + 1;
                }
                if (valid) memcpy(custom_shades16, rgb, sizeof rgb);
            }
            else if (!strcmp(line, "screen")) screen_scale = d <= GRAPHICS_SCALE_STRETCH ? (uint8_t)d : screen_scale;
            else if (!strncmp(line, "palette", 7) || !strncmp(line, "custom", 6)) {
                const bool custom = line[0] == 'c';
                const unsigned layer = strtoul(line + (custom ? 6 : 7), nullptr, 10);
                if (layer >= LAYER_COUNT) continue;
                if (custom) parse_rgb4(val, custom_rgb[layer]);
                else palette_index[layer] = d <= PALETTE_CUSTOM ? (uint8_t)d : PALETTE_AUTO;
            }
        }
        if (palette_list < PALETTE_LIST_VERSION) {
            palette_all = palette_from_list1(palette_all);
            for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
                palette_index[layer] = palette_from_list1(palette_index[layer]);
        }
    }
    else if (f_open(&f, "/GB/gb.conf", FA_READ) == FR_OK) {
        // the settings of an older version: swap AB, palette (0..12 as now), TV system, screen
        UINT br;
        uint8_t v = 0;
        f_read(&f, &swap_ab, 1, &br);
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v < PALETTE_MONO_FIRST)
            palette_all = v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= 1) tv_system = v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= GRAPHICS_SCALE_STRETCH) screen_scale = v;
        f_close(&f);
    }
#if HDMI
    gray_lines_menu = gray_lines ? 1 : 0;
    gray_lines = gray_lines_menu ? GRAPHICS_GRAY_HORIZONTAL : 0;
#endif
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) randomize_layer(layer);
}

static void f_save_conf(void) {
    char pathname[96];
    config_mkdirs();
    config_path(pathname, sizeof pathname);
    static char text[768];
    palette16_global = palette16_select;
    int len = snprintf(text, sizeof text, "swap_ab=%u\r\ntv_system=%u\r\nscreen=%u\r\npalette_all=%u\r\nlcd_ghosting=%u\r\ndemo_duration=%u\r\ndemo_name=%u\r\ndemo_timer_osd=%u\r\n",
                       (unsigned)swap_ab, (unsigned)tv_system, (unsigned)screen_scale, (unsigned)palette_all,
                       (unsigned)lcd_ghosting, (unsigned)demo_duration, (unsigned)demo_name,
                       (unsigned)demo_timer_osd);
    for (unsigned i = 0; i < LAYER_COUNT && len > 0 && len < (int)sizeof text; i++)
        len += snprintf(text + len, sizeof text - len, "palette%u=%u\r\ncustom%u=%06lX,%06lX,%06lX,%06lX\r\n",
                        i, (unsigned)palette_index[i], i,
                        (unsigned long)custom_rgb[i][0], (unsigned long)custom_rgb[i][1],
                        (unsigned long)custom_rgb[i][2], (unsigned long)custom_rgb[i][3]);
    if (len > 0 && len < (int)sizeof text)
        len += snprintf(text + len, sizeof text - len, "palette16=%u\r\ncustom16=", (unsigned)palette16_global);
    for (unsigned i = 0; i < 16 && len > 0 && len < (int)sizeof text; i++)
        len += snprintf(text + len, sizeof text - len, "%s%06lX",
                        i ? "," : "", (unsigned long)custom_shades16[i]);
    if (len > 0 && len < (int)sizeof text)
        len += snprintf(text + len, sizeof text - len, "\r\n");
    if (len > 0 && len < (int)sizeof text)
        len += snprintf(text + len, sizeof text - len, "tv_color=%u\r\ngray_lines=%u\r\ngray_level=%u\r\npalette_list=%u\r\n",
                        (unsigned)tv_color, (unsigned)gray_lines, (unsigned)gray_level, PALETTE_LIST_VERSION);
#if BACKPLANE
    if (len > 0 && len < (int)sizeof text)
        len += snprintf(text + len, sizeof text - len, "backplane=%u\r\n", (unsigned)backplane);
#endif
    if (len <= 0 || len >= (int)sizeof text) return;
    static FIL f; // static: core 0 has a small stack
    if (f_open(&f, pathname, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
    UINT bw;
    f_write(&f, text, (UINT)len, &bw);
    f_close(&f);
}

/* Palette import: six-hex-digit RGB tokens separated by arbitrary non-hex
 * characters. Runs of other lengths (including words) are ignored, not split.
 * Read at most sixteen colours; the only valid totals are 4, 12 and 16.
 * 4 colours (a .hex file of lospec.com) go to all three palettes. */
static palette_import_result_e import_palette_file(const char* pathname) {
    static FIL file;         // core 0 has a small stack
    static char chunk[256];
    if (f_open(&file, pathname, FA_READ) != FR_OK) return PAL_IMPORT_IO;

    uint32_t colours[16];
    unsigned found = 0;
    unsigned digits = 0;
    uint32_t value = 0;
    FRESULT fr = FR_OK;
    UINT nr = 0;
    bool eof = false;
    while (found < 16 && !eof) {
        fr = f_read(&file, chunk, sizeof chunk, &nr);
        if (fr != FR_OK || nr == 0) {
            eof = true;
            break;
        }
        for (UINT i = 0; i < nr && found < 16; i++) {
            const unsigned char c = (unsigned char)chunk[i];
            const int hex = c >= '0' && c <= '9' ? c - '0' :
                            c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                            c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (hex >= 0) {
                if (digits < 6) value = (value << 4) | (unsigned)hex;
                if (digits < 7) digits++; // >6 hex digits is not an RGB token
            } else {
                if (digits == 6) colours[found++] = value;
                digits = 0;
                value = 0;
            }
        }
    }
    const FRESULT close_fr = f_close(&file);
    if (fr != FR_OK || close_fr != FR_OK) return PAL_IMPORT_IO;
    if (eof && digits == 6 && found < 16) colours[found++] = value;
    if (found != 4 && found != 12 && found != 16) return PAL_IMPORT_FORMAT;
    if (found == 4)
        for (unsigned i = 4; i < 12; i++) colours[i] = colours[i & 3];

    if (found == 4 || found == 12) {
        memcpy(custom_rgb, colours, sizeof custom_rgb);
        palette16_select = 0; // 12 colours: normal 3x4 palettes
    } else {
        memcpy(custom_shades16, colours, sizeof custom_shades16);
        palette16_select = PALETTE16_CUSTOM; // 16 colours: 4x4 shade transitions
    }
    palette_all = PALETTE_CUSTOM;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        palette_index[layer] = PALETTE_CUSTOM;
    if (gb_initialized && !gb.cgb.cgbMode) update_palette();
    f_save_conf();
    return found == 4 ? PAL_IMPORT_4 : found == 12 ? PAL_IMPORT_12 : PAL_IMPORT_16;
}

static void menu_frame() {
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
}

/* the palettes are chosen one by one only under "Palette: CUSTOM" */
static bool menu_item_selectable(const unsigned i) {
    const MenuItem& item = menu_items[i];
    if (item.type == NONE) return false;
    // Save states, per-game colours and the editor require a valid GB context.
    if (!gb_initialized &&
        (item.callback == &save || item.callback == &load ||
         item.type == GAME_INI || item.type == PALETTE_EDITOR)) return false;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        if (item.value == &palette_index[layer]) return palette_all == PALETTE_CUSTOM;
#if BACKPLANE
    if (item.value == &backplane) return screen_scale != GRAPHICS_SCALE_STRETCH; // Full has none
#endif
#if VGA
    if (item.value == &gray_lines) return screen_scale != GRAPHICS_SCALE_STRETCH;
    if (item.value == &gray_level) return screen_scale != GRAPHICS_SCALE_STRETCH && gray_lines;
#elif HDMI
    if (item.value == &gray_lines_menu) return screen_scale != GRAPHICS_SCALE_STRETCH;
    if (item.value == &gray_level) return screen_scale != GRAPHICS_SCALE_STRETCH && gray_lines_menu;
#endif
    return true;
}

void menu() {
    bool exit = false;
#if VGA || HDMI || SOFTTV
    // Keep the REC bar out of the text menu, resume it at the next game frame.
    if (replay_mode == REPLAY_RECORD) graphics_set_overlay(nullptr, false);
    replay_osd_second = UINT32_MAX;
#endif
    graphics_set_mode(TEXTMODE_DEFAULT);
    menu_frame();
    uint current_item = 0;
    /* Only fresh presses count: the Start+Select that opened the menu must be
     * released first, so it can't change or activate the first item. */
    uint8_t prev_keys = input_held();

    filebrowser_toggle_requested = false; // a press before the menu was drawn
    while (!exit) {
        if (filebrowser_toggle_requested) { // F10: the next step of the cycle (the game or the browser)
            filebrowser_toggle_requested = false;
            break;
        }
        const uint8_t held_keys = input_held();
        const uint8_t pressed = held_keys & (uint8_t)~prev_keys;
        prev_keys = held_keys;
        const bool activate = pressed & BTN_START;
        const bool dmg = gb_initialized && !gb.cgb.cgbMode; // only a loaded DMG game

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
                            const uint8_t old = *value;
                            if ((pressed & BTN_RIGHT) && *value < item->max_value) {
                                (*value)++;
                            }
                            if ((pressed & BTN_LEFT) && *value > 0) {
                                (*value)--;
                            }
                            for (unsigned layer = 0; layer < LAYER_COUNT; layer++) {
                                if (item->value != &palette_index[layer] && item->value != &palette_all)
                                    continue;
                                // Start on RANDOM draws new colours
                                if (layer_selector(layer) == PALETTE_RANDOM && (old != *value || activate))
                                    randomize_layer(layer);
                                if (old != *value || activate) {
                                    if (old != *value) palette16_select = 0;
                                    if (gb_initialized) update_palette();
                                }
                            }
                            if (item->value == &palette16_select && (old != *value || activate) && gb_initialized)
                                update_palette();
                        }
                        break;
                    case PALETTE_EDITOR:
                        if (activate && dmg) {
                            palette16_select = 0;
                            update_palette();
                            palette_editor(true);
                            menu_frame();
                            prev_keys = input_held(); // the key that closed the editor
                            filebrowser_toggle_requested = false; // F10 there is not the cycle
                        }
                        break;
                    case RETURN:
                        if (activate)
                            exit = true;
                        break;

                    case START_DEMO:
                        if (activate) {
                            demo_requested = true;
                            demo_advance_pending = false;
                            f_save_conf();
                            apply_video_settings();
                            restart = true;
                            return;
                        }
                        break;
                    case ROM_SELECT:
                        if (activate) {
                            demo_active = demo_requested = demo_advance_pending = false;
                            f_save_conf(); // this exit used to lose the Screen setting
                            apply_video_settings();
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
                    if (!menu_item_selectable(i)) {
                        // a palette that follows "Palette:" shows its selector
                        const bool layer = item->value != &palette_all && item->max_value == PALETTE_CUSTOM;
                        snprintf(result, TEXTMODE_COLS, item->text,
                                 item->value_list[layer ? palette_all : *(uint8_t *)item->value]);
                        color = 6;
                        break;
                    }
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *)item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                case GAME_INI:
                    snprintf(result, TEXTMODE_COLS, "%s",
                             game_ini_linked ? "Unlink game color file   " : "Save colors for this game");
                    if (!dmg || !rom_file[0]) color = 6;
                    break;
                case PALETTE_EDITOR:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
                    if (!dmg) color = 6;
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
        }

        if (pressed & BTN_DOWN) {
            do {
                current_item = (current_item + 1) % MENU_ITEMS_NUMBER;
            } while (!menu_item_selectable(current_item));
        }
        if (pressed & BTN_UP) {
            do {
                current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;
            } while (!menu_item_selectable(current_item));
        }

        sleep_ms(25);
    }
    palette_hotkeys_clear();
    if (gb_initialized) {
        palette_apply();
        palette16_apply();
    }
#if HDMI
    gray_lines = gray_lines_menu ? GRAPHICS_GRAY_HORIZONTAL : 0;
#endif
    f_save_conf();
    apply_video_settings(); // the TV system and colour: applied by graphics_set_mode()
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}

#if HWAY
/* Without the I2S DMA nothing waits for the sound: keep the Game Boy frame
 * rate (70224 cycles of 4194304 Hz = 16742.7 us) by the timer.  After a
 * stop (menu, file browser, slow frames) the count starts again. */
static void hway_frame_wait() {
    static uint64_t t0;
    static uint32_t frames;
    const uint64_t now = time_us_64();
    uint64_t target = t0 + (uint64_t)(frames + 1) * 70224000000ull / 4194304u;
    if (!t0 || now > target + 50000u) {
        t0 = now;
        frames = 0;
        return;
    }
    while (time_us_64() < target) tight_loop_contents();
    if (++frames >= 4194304u) { // keep the product in range: restart from here
        t0 = target;
        frames = 0;
    }
}
#endif

/* A new game must not inherit anything the display still holds from the old
 * one: the last frame (front or still pending), the LCD ghosting history, the
 * Game Boy Color palette sets of a frame.  Without this the old picture stays
 * on the screen while the new ROM is loaded, and the colours of the new game
 * (a DMG palette goes to the video driver at once) are applied to it. */
static void game_video_reset() {
    critical_section_enter_blocking(&frame_lock);
    frame_pending = -1;     // an unshown frame of the old game is dropped
    frame_started = false;
    critical_section_exit(&frame_lock);
    memset(SCREEN, 0, sizeof SCREEN);
    memset(SCREEN_2, 0, sizeof SCREEN_2);
    memset(frame_sets, 0, sizeof frame_sets);
    memset(blend_prev, 0, sizeof blend_prev);
    // colour 0 is black until the new game's palette is in place
    graphics_set_palette(0, RGB888(0, 0, 0));
    graphics_set_buffer((uint8_t *)frames[frame_front], LCD_WIDTH, LCD_HEIGHT);
}

int main() {
    detect_flash_size();
    overclock();
    /* QMI CS1 setup before core1 starts: the probe briefly blocks XIP */
    gb_psram_init();

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sem_init(&vga_start_semaphore, 0, 1);
    critical_section_init(&frame_lock);
    cube_pal_init();
    blend_slots_init();
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

#if HWAY
    hway_init(); // the AY chips play the sound, nothing paces the emulator
#else
    // Initialize I2S sound driver
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
#endif

    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        /// TODO: error handling
        while(1);
    } else {
        f_mkdir(HOME_DIR);
        f_load_conf();
        apply_video_settings();
    }

    bool browser_rom_preloaded = false;
    while (true) {
        // A ROM switch, menu reset or Demo selection ends an old movie.
        replay_stop();
        // The previous game's state is no longer running.  Only a successful
        // gb_init() below permits entry into the emulator's frame loop.
        gb_initialized = false;
#if VGA || HDMI || SOFTTV
        graphics_set_overlay(nullptr, false);
#endif
        /* The automatic demonstration advances without opening a browser.
         * The regular F10 path sets browser_rom_preloaded for a manual ROM. */
        bool demo_loaded = false;
        if (demo_requested || (demo_active && demo_advance_pending))
            graphics_set_mode(TEXTMODE_DEFAULT);
        if (demo_requested) {
            demo_requested = false;
            demo_loaded = demo_begin();
        } else if (demo_active && demo_advance_pending) {
            demo_advance_pending = false;
            demo_loaded = demo_load_next_rom(demo_current_name);
            if (!demo_loaded) {
                // The last playable ROM of the directory has been shown:
                // finish the Demo and fall through to the file browser
                // (no wrap-around to the first ROM).
                demo_active = false;
                // Put the browser cursor on the ROM the Demo ended with,
                // if it is in the directory the browser is showing.
                if (demo_current_name[0] && strcmp(demo_dir, fb_dir) == 0)
                    snprintf(fb_focus, sizeof fb_focus, "%s", demo_current_name);
            }
        }
        if (FR_OK == fr && !browser_rom_preloaded && !demo_loaded) {
            fb_rom_ready = false; // a previous browser selection is not a new launch
            graphics_set_mode(TEXTMODE_DEFAULT);
#if BACKPLANE
            filebrowser("gbc,gb,gbbp"); // .gbbp: a backplane to install
#else
            filebrowser("gbc,gb");
#endif
            if (demo_requested) { // B from the browser
                demo_requested = false;
                demo_loaded = demo_begin();
                if (!demo_loaded) continue; // let the user choose another folder
            }
        }
        const bool cartridge_selected = browser_rom_preloaded || demo_loaded || fb_rom_ready;
        browser_rom_preloaded = false;

        // Browser settings are accessible without any loaded cartridge.
        // Do NOT initialise (or run) a nonexistent previous ROM just to open
        // the menu.  On Return, visit the ROM browser instead of running GB.
        if (fb_menu_requested) {
            fb_menu_requested = false;
            menu();
            restart = false;
            continue;
        }
        if (!cartridge_selected) continue;

        game_video_reset(); // nothing of the previous game on the screen
        graphics_set_mode(GRAPHICSMODE_DEFAULT);
        /* Initialise GB context. */
        cgb_palette_reset();
        const gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                            &gb_cart_ram_write, &gb_error, nullptr);
        if (ret != GB_INIT_NO_ERROR) {
            // A bad/absent cartridge is not a fatal firmware error.  No GB
            // frames, cart RAM or per-game palettes may be accessed here.
            printf("E gb_init error: %u\n", (unsigned)ret);
            demo_active = demo_requested = demo_advance_pending = false;
            graphics_set_mode(TEXTMODE_DEFAULT);
            draw_text(ret == GB_INIT_INVALID_CHECKSUM
                          ? "Game Boy ROM: invalid header checksum"
                          : "Invalid or unsupported Game Boy ROM", 1, 1, 12, 0);
            sleep_ms(1200);
            restart = false;
            continue;
        }
        gb_initialized = true;
        /* A new APU for every start: the channels of the previous game
         * (or of this one before a reset) must not sound in the first frames. */
        audio_init();

        /* The colours of a Game Boy game: the palette saved for it, or the selected one */
        apply_game_palette_on_load();
        palette_hotkeys_clear();

        gb_init_lcd(&gb, &lcd_draw_line);
        /* Load Save File. */
        read_cart_ram_file(&gb);
        if (replay_launch != REPLAY_LAUNCH_NORMAL) {
            const bool record = replay_launch == REPLAY_LAUNCH_RECORD;
            replay_launch = REPLAY_LAUNCH_NORMAL;
            if (!replay_start(record)) {
                graphics_set_mode(TEXTMODE_DEFAULT);
                draw_text(record ? "Could not start recording" : "Replay missing or incompatible", 1, 1, 12, 0);
                sleep_ms(1500);
                continue;
            }
        }

        // the Enter/Start that launched the ROM must not reach the game
        input_gate_t game_gate;
        if (fb_menu_requested) {
            fb_menu_requested = false;
            menu();
            cgb_palette_reset(); // the menu may have changed the video palette
        }
        game_gate.arm();

        //=============================================================================
        while (!restart) {
            /* F10: open the same ROM browser without discarding the running GB.
             * F10 again restores that exact emulator state; selecting a ROM
             * restarts only the GB context, not the whole firmware. */
            if (filebrowser_toggle_requested) {
                filebrowser_toggle_requested = false;
                fb_can_cancel = true;
                fb_canceled = false;
                fb_rom_ready = false;
                // Stop the demo clock while F10 has the screen and SD card.
                const bool was_demo = demo_active;
                const bool pending_demo = demo_advance_pending;
                const uint64_t browser_started_at = time_us_64();
                demo_active = false;
                demo_advance_pending = false;
                // FatFs replay file must be closed before the ROM browser or
                // its PSRAM/flash loader touches the SD card.
                replay_stop();
                graphics_set_mode(TEXTMODE_DEFAULT);
#if BACKPLANE
                filebrowser("gbc,gb,gbbp");
#else
                filebrowser("gbc,gb");
#endif
                fb_can_cancel = false;
                graphics_set_mode(GRAPHICSMODE_DEFAULT);
                if (fb_canceled && rom_image_damaged) {
                    // A failed ROM replacement may have overwritten the game's
                    // PSRAM/flash bytes.  Returning to it would run corrupt code.
                    fb_canceled = false;
                    demo_active = demo_requested = demo_advance_pending = false;
                    restart = true;
                    break;
                }
                if (fb_canceled) {
                    fb_canceled = false;
                    if (fb_f10_menu) { // F10 in the browser: the settings, then the game
                        fb_f10_menu = false;
                        menu();
                        cgb_palette_reset(); // the menu may have changed the video palette
                        if (restart) break;  // Start Demo / another ROM from the menu
                    }
                    demo_active = was_demo;
                    demo_advance_pending = pending_demo;
                    if (was_demo && demo_game_started_at)
                        demo_game_started_at += time_us_64() - browser_started_at;
                    // The browser writes into SCREEN; the next GB frame
                    // repaints it if SCREEN is currently the front buffer.
                    graphics_set_buffer((uint8_t *)frames[frame_front], LCD_WIDTH, LCD_HEIGHT);
                    game_gate.arm(); // don't pass the held browser keys to GB
                } else {
                    // A manually selected game must not inherit the Demo timer.
                    demo_active = false;
                    demo_advance_pending = false;
                    // Browser loaded the next cartridge, or requested Demo.
                    browser_rom_preloaded = fb_rom_ready;
                    restart = true;
                    break;
                }
            }
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
            // The menu is a host action, never part of the recorded GB input.
            //------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            static int keydown_counter = 0;
            if ((keys & (BTN_SELECT | BTN_START)) == (BTN_SELECT | BTN_START) ||
                (nespad_state & DPAD_X)) {
                char romname[24];

                gb_get_rom_name(&gb, romname);

                if (nullptr != strstr(romname, "ZELDA")) {
                    // half a second (the game itself uses Start+Select)
                    if (keydown_counter++ > 30) {
                        menu();
                        cgb_palette_reset(); // the menu may have changed the video palette
                        keydown_counter = 0;
                        game_gate.arm();
                    }
                }

                else {
                    menu();
                    cgb_palette_reset(); // the menu may have changed the video palette
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

            if (handle_palette_hotkeys())
                game_gate.arm(); // the keys that closed the palette editor

            // Replay input belongs to the emulated frame AFTER all host/menu
            // actions; loading a saved state can rewind replay_frame above.
            const uint8_t game_keys = replay_begin_frame(game_gate.read());
            gb.direct.joypad_bits.up = !(game_keys & BTN_UP);
            gb.direct.joypad_bits.down = !(game_keys & BTN_DOWN);
            gb.direct.joypad_bits.left = !(game_keys & BTN_LEFT);
            gb.direct.joypad_bits.right = !(game_keys & BTN_RIGHT);
            gb.direct.joypad_bits.a = !(game_keys & BTN_A);
            gb.direct.joypad_bits.b = !(game_keys & BTN_B);
            gb.direct.joypad_bits.select = !(game_keys & BTN_SELECT);
            gb.direct.joypad_bits.start = !(game_keys & BTN_START);

            //-----------------------------------------------------------------
            gb_debug_phase = 1;
#if !HWAY
            gb_audio_cycles = 0;
            audio_frame_begin(reinterpret_cast<int16_t *>(stream));
#endif
            gb_run_frame(&gb);
            gb_debug_phase = 2;
#if !HWAY
            audio_frame_end();
#endif
            replay_end_frame();

            //gb.direct.interlace = 1;

            if (!gb.direct.frame_skip) {
#if HWAY
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                audio_hway_update();
                hway_frame_wait();
#else
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
#endif
            }
            gb_debug_phase = 0;
            ++gb_debug_frame_id;
#if VGA || HDMI || SOFTTV
            if (replay_mode == REPLAY_RECORD) replay_draw_record_overlay();
            else demo_draw_title_overlay();
#endif
            if (demo_active && time_us_64() - demo_game_started_at >=
                               (uint64_t)demo_seconds[demo_duration] * 1000000ull) {
                demo_advance_pending = true;
                restart = true;
            }
        }
        restart = false;
    }
}
