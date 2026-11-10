# pico-gameboy

**Nintendo Game Boy / Game Boy Color emulator for RP2350-based MURMULATOR
systems and compatible boards.**

`pico-gameboy` runs Game Boy software on Raspberry Pi Pico 2 / RP2350-class
hardware with several video and audio backends for MURMULATOR-style
retro-computer hardware. It is based on the
[Peanut-GB](https://github.com/deltabeard/Peanut-GB) core and the
[minigb_apu](https://github.com/baines/MiniGBS) sound emulation; the original
port is [xrip/pico-gameboy](https://github.com/xrip/pico-gameboy).

Current version: **3.2.9**

## Supported boards

| Board | Firmware prefix | Video | Sound |
|---|---|---|---|
| MURMULATOR 1.x (also MURMULATOR Ultimate v2.x) | `m1p2` | VGA, HDMI, TV-SOFT | PWM, I2S (TDA1387), AY-3-8910 |
| MURMULATOR 2.0 | `m2p2` | VGA, HDMI | PWM, I2S (TDA1387) |
| Olimex RP2040-PICO-PC with a Pico 2 | `PCp2` | HDMI (DVI connector) | PWM (3.5 mm jack) |
| Waveshare RP2350-PiZero | `z0p2` | mini-HDMI | I2S (an external PCM5102A module on GP10..12) |

The release builds target **RP2350 / ARM Cortex-M33**. RP2040 builds
(`m1p1`, `m2p1`, `PCp1`) still compile, without the backplane (no room in
RAM).

## Features

- Game Boy and Game Boy Color emulation (Peanut-GB)
- SD-card cartridge browser (`.gb`, `.gbc`)
- PS/2 and USB keyboards, NES gamepads
- VGA, HDMI and software PAL/NTSC composite output (**TV-SOFT**)
- PWM, I2S and hardware AY-3-8910 / TurboSound sound
- QSPI PSRAM for cartridges on RP2350 modules that have it
- screen sizes 2x, 3x and full screen; a Game Boy Color backplane around
  the game in 2x and 3x, replaceable from the SD card (`.gbbp`)
- colours for Game Boy (not Color) games: palettes for the background and
  both sprite palettes, random palettes, 16-shade presets, a palette editor,
  per-game palette presets
- LCD ghosting: the flicker-based transparency of Game Boy games
- gray lines between the pixels (the LCD grid) on VGA and HDMI
- Game Boy Color pictures that change palettes in the middle of a frame
  (hi-colour pictures)
- save states (8 slots)
- Demo mode with automatic cartridge switching
- input recording and playback by emulated frame (`.gbr`), with save-state
  branching
- persistent settings, per board and video output

## Controls

### Keyboard

| Key | Game Boy |
|---|---|
| Arrows, WASD, numeric keypad 1–9 (7, 9, 1, 3 - diagonals) | D-pad |
| Z, O, keypad 0 | A |
| X, P, keypad . | B |
| Enter, keypad Enter | START |
| Backspace, Esc, keypad + | SELECT |
| Enter + Esc (START + SELECT) | emulator menu |

Emulator shortcuts in a game:

| Key | Action |
|---|---|
| Ctrl+F1 … Ctrl+F8 | save the game state to slot 1–8 |
| Alt+F1 … Alt+F8 | load the game state from slot 1–8 |
| F1, F2, F3 | a new random palette for the background, sprites 0, sprites 1 |
| F6 | new random palettes for all three |
| F7 | new random palettes, saved for this game |
| F8 | save the colours shown now for this game |
| F9 | all palettes back to AUTO |
| F12 | palette editor |
| F10 | the cartridge browser; F10 there — the settings, F10 again — back to the game |
| Ctrl+Alt+Delete | reset the board |

### NES gamepad

A — A, B — B, Start — START, Select — SELECT, Start+Select — menu.

### Cartridge browser

| Key | Action |
|---|---|
| Arrows | move |
| PageUp / PageDown, Home / End | by a page, to the beginning / end |
| START (Enter) | run the cartridge, enter a directory |
| SELECT (Backspace) | the previous cartridge |
| B (X) | Demo mode for this directory |
| R | record the input of the selected cartridge |
| P | play the recording of the selected cartridge |
| F10 | the settings (then F10 — back to the running game, or to the browser without one) |
| START + SELECT | settings |

Selecting a `.gbbp` file installs it as the backplane (see below).

## Emulator menu

| Item | Meaning |
|---|---|
| Swap AB <> BA | swap the A and B buttons |
| Palette, BG / OBJ0 / OBJ1 | colours of Game Boy games: one selector for all three palettes, or each palette separately under CUSTOM |
| 16 shades | a 16-shade preset for Game Boy games (OFF — the palettes above) |
| Palette editor | edit the 12 colours |
| (game preset) | link / unlink the colours to this game |
| LCD ghosting | mix every Game Boy frame with the previous one |
| Demo time, Demo name OSD, Demo timer OSD | Demo mode settings |
| Save state / Load state | the slot |
| TV system, Colors | TV-SOFT: PAL / NTSC, colour or black and white |
| Screen | 2x, 3x, Full |
| Backplane | the picture around the game (2x and 3x) |
| Gray lines, Gray level | the LCD grid (VGA, HDMI; 2x and 3x) |
| Overclocking | the CPU clock |
| Start Demo, Reset to ROM select, Return to game | |

## Colours of Game Boy games

A Game Boy game has three 4-shade palettes: the background (BG) and two
sprite palettes (OBJ0, OBJ1). **Palette** sets all three: AUTO (the colours
a Game Boy Color gives this game), the Game Boy Color boot palettes, the
Super Game Boy palettes of
[lospec.com](https://lospec.com/palette-list/nintendo-super-gameboy), the
pico-gamate 4-colour palettes, RANDOM or CUSTOM; with CUSTOM each palette
has its own selector. The **palette editor** (F12) shows all 12 colours:
arrows choose a colour, Enter / A edits it (left / right — R, G, B, up /
down — the value, or type `RRGGBB`), Esc / B goes back, START / F12 closes
the editor. Editing a colour turns its palette into CUSTOM.

A palette file (`.txt`, `.csv` or `.hex`) selected in the ROM browser
replaces the CUSTOM colours: the `RRGGBB` values in it are read in order,
4 of them (a lospec.com `.hex` file) go to all three palettes, 12 — BG,
OBJ0, OBJ1, 16 — the CUSTOM 16-shade preset.

F7 and F8 save the colours of a game as `/.config/gbc/<ROM file>.ini`; the
preset is applied whenever that cartridge is started.

**16 shades** replaces the palettes with a 16-shade preset; with LCD
ghosting the transition from one shade to another gets its own colour.

Game Boy Color games keep their own colours.

## Display

- **Screen**: 2x (160×144 → 320×288 pixels), 3x (480×432) or Full (the
  whole 640×480 screen). TV-SOFT uses the same choices.
- **Backplane**: a photographic Game Boy Color case around the game in 2x
  and 3x (HDMI, VGA, TV-SOFT on RP2350). Another backplane is installed by
  selecting a `.gbbp` file in the cartridge browser: "Backplan is installed"
  — it is copied to `/.config/gbc/backplane.gbbp` and shown from then on;
  "This backplan version is unsupported" — a file of another version. Delete
  that file to get the built-in one back. Backplanes are made from any
  picture by `tools/make_gbbp.py` ([`tools/README.md`](tools/README.md));
  ready ones are in `sd-card/GB`.
- **LCD ghosting**: the Game Boy screen is slow, so games draw a sprite every
  second frame to make it half-transparent. The emulator mixes every frame
  of a Game Boy (not Color) game with the previous one; without it such
  sprites flicker.
- **Gray lines**: the gaps between the Game Boy pixels drawn in grey, like
  the grid of the real LCD; **Gray level** 0–3 — black, dark grey, light
  grey, white. VGA: No / Vertical / Horizontal / Both. HDMI: horizontal
  lines only (OFF / ON) — an HDMI sample is two pixels wide. Not in Full.
- **TV-SOFT**: PAL or NTSC; **Colors: NO** sends no colour subcarrier (a
  black and white picture).

## Audio

| Build suffix | Output |
|---|---|
| `PWM` | PWM on the board's sound pins |
| `I2S` | an I2S DAC (TDA1387 on MURMULATOR, PCM5102A module on RP2350-PiZero) |
| `I2S-CS4334` | an I2S DAC CS4334 |
| `AY-3-8910` | hardware AY-3-8910 / TurboSound (MURMULATOR 1.x only) |

The AY build maps the Game Boy channels onto two AY chips: the two squares
and the wave channel (as a square of its pitch) to the tones of the first
chip, the noise to the noise of the second one. The duty cycle, the wave
shape and the short (7-bit) noise are lost; pitch and volume are kept. The
AY chips are clocked at 1.75 MHz from GP21.

## Cartridge storage

A selected cartridge is loaded into QSPI PSRAM when the RP2350 module has it
(the browser header shows `QSPI PSRAM`), otherwise it is written to flash
right after the firmware (`FLASH`); the last 512 KB of flash (pico-launcher)
are never touched. Sectors that already hold the same data are not
rewritten.

## Demo mode

**Start Demo** in the menu, or B in the browser, plays the cartridges of a
directory one after another, each for the **Demo time** (15 s … 10 min).
**Demo name OSD** shows the title for the first seconds, **Demo timer OSD**
the remaining time. Any manually chosen cartridge ends the demo.

## Input recording and playback

R in the browser starts the selected cartridge and records the joypad by
emulated frame (`REC` and the time are shown at the bottom); P plays the
recording back. Recordings are stored as
`/.config/gbc/replay/<ROM title>-<tag>.gbr`; a new recording replaces the
previous one. A save state made while recording remembers its place in the
recording: loading it truncates the recording there and continues along a
new branch.

## Save states

Ctrl+F1 … Ctrl+F8 save, Alt+F1 … Alt+F8 load (or the Save state / Load
state items of the menu). States are kept in `/.config/gbc`.

## SD-card layout

```
/GB/                                      cartridges (.gb, .gbc), backplanes (.gbbp)
/.config/gbc/<board>/<video>/config.ini  settings
/.config/gbc/<ROM file>.ini               the colours of a game
/.config/gbc/<ROM title>[_<slot>].save    save states
/.config/gbc/replay/<title>-<tag>.gbr     input recordings
/.config/gbc/backplane.gbbp               the installed backplane
```

`<board>` is `m1p2`, `m2p2`, `pcp2` or `z0p2`, `<video>` — `vga`, `hdmi`,
`softtv`, `tv` or `tft`. The settings of older versions (`/GB/gb.conf`) are
read once if there is no `config.ini`.

## Configuration format

`config.ini` is `key=value` text (the emulator writes it when the menu is
closed): `swap_ab`, `tv_system`, `tv_color`, `screen`, `palette_all`,
`palette0..2`, `custom0..2` (`RRGGBB,RRGGBB,RRGGBB,RRGGBB`), `palette16`,
`lcd_ghosting`, `demo_duration`, `demo_name`, `demo_timer_osd`,
`backplane`, `gray_lines`, `gray_level`, `palette_list` (the numbering of
the palettes: without `palette_list=2` the numbers after DMG are those
from before the Super Game Boy palettes and are moved up by 30). Unknown
keys are ignored.

## Building

The [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0
is required (or the Raspberry Pi Pico VS Code extension).

```sh
cmake -S . -B build -DPICO_BOARD=murmulator -DVGA=ON -DHDMI=OFF -DI2S=OFF
cmake --build build
```

| Option | Meaning |
|---|---|
| `PICO_BOARD` | `murmulator` (default), `murmulator2`, `olimex-pico-pc`, `waveshare_rp2350_pizero` |
| `PICO_PLATFORM` | `rp2350` (default) or `rp2040` |
| `HDMI` (default ON), `VGA`, `SOFTTV`, `TV`, `TFT` (+ `ILI9341`) | the video output; the first one set wins in the order TFT, HDMI, TV, SOFTTV, otherwise VGA |
| `I2S` (default ON), `I2S_CS4334`, `HWAY` | the sound output; with all OFF — PWM |
| `m1p2launcher` | the m1p2-launcher memory map |

### Build all release configurations

`build-all.sh` (Linux, macOS, Git Bash) and `build-all-rp2350.bat` (Windows,
with the Pico VS Code extension's CMake and Ninja) build every supported
RP2350 variant into `bin/rp2350/Release/`:

| Board | Video | Sound |
|---|---|---|
| `murmulator` | VGA, HDMI, SOFTTV | PWM, I2S, HWAY |
| `murmulator2` | VGA, HDMI | PWM, I2S |
| `olimex-pico-pc` | HDMI | PWM |
| `waveshare_rp2350_pizero` | HDMI | I2S |

`./build-all.sh murmulator` builds the variants of one board.

## Firmware file names

`<prefix>-gameboy-color-<video>-<sound>-<version>.uf2`, for example
`m1p2-gameboy-color-VGA-I2S-3.2.9.uf2`:

- prefix: `m1p2`, `m2p2`, `PCp2`, `z0p2` (RP2040: `m1p1`, `m2p1`, `PCp1`);
- video: `VGA`, `HDMI`, `TV-SOFT`, `TV`, `TFT-ST7789`, `TFT-ILI9341`;
- sound: `PWM`, `I2S`, `I2S-CS4334`, `AY-3-8910`.

## Installing firmware

Hold BOOTSEL, connect the Pico to a computer and copy the `.uf2` file to the
`RP2350` drive, or start it from a firmware manager (pico-launcher, MOS2).
The CPU runs at 378 MHz / 1.6 V (RP2350-PiZero: 252 MHz); **Overclocking**
in the menu raises it further.

## Default hardware pin assignments

| | MURMULATOR 1.x | MURMULATOR 2.0 | Olimex PICO-PC | RP2350-PiZero |
|---|---|---|---|---|
| VGA / HDMI | GP6..13 | GP12..19 | GP12..19 | GP32..39 |
| SD card (SCK, MOSI, MISO, CS) | 2, 3, 4, 5 | 6, 7, 4, 5 | 6, 7, 4, 22 | 30, 31, 40, 43 |
| PS/2 keyboard | GP0, GP1 | GP2, GP3 | GP0, GP1 | GP2, GP3 |
| NES gamepad (CLK, LAT, DATA) | 14, 15, 16 | 20, 21, 26 | 5, 9, 20 | 4, 5, 7 |
| PWM sound | GP26, GP27 | GP10, GP11 | GP28 (left), GP27 (right) — jack | — |
| I2S (DATA, BCK, LRCK) | 26, 27, 28 | 9, 10, 11 | — | 10, 11, 12 |
| QSPI PSRAM CS (RP2350A / B) | GP19 / GP47 | GP8 / GP47 | GP8 / GP47 | GP8 / GP47 |
| AY-3-8910 (latch, clock, data; AY clock) | 26, 27, 28; GP21 | — | — | — |

The pins are defined in `boards/*.h`.

## Source tree

```
boards/          board pin definitions
drivers/         video (vga-nextgen, hdmi, tv-software, tv, st7789), graphics
                 (backplane), audio, SD card, FatFs, PS/2, NES gamepad
ext/minigb_apu/  Game Boy sound
inc/             Peanut-GB, palettes
src/             the emulator (main.cpp), USB keyboard, PSRAM, HW AY
sd-card/GB/      ready backplanes
tools/           make_gbbp.py — backplanes from pictures
```

## Release notes

See [`CHANGELOG.md`](CHANGELOG.md).

## Notes

- No copyrighted games are included; use cartridges you legally own.
- Accuracy is traded for speed in places; some games may not work as
  expected.
- Repeatedly writing cartridges to flash wears it (≥ 100K erase cycles);
  PSRAM avoids that.
- Overclocking and raised voltage are outside the manufacturer's guaranteed
  modes.

## License

MIT, see [`LICENSE`](LICENSE).
