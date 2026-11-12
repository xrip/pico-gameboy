# v3.2.9
- F10 cycles game -> ROM browser -> settings -> game (without a game: browser -> settings -> browser)
- 30 Super Game Boy palettes from lospec.com after DMG; the palette numbers in config.ini and the game .ini files of older versions are moved up
- palette file import: .hex files too, 4 colours go to all three CUSTOM palettes
- more colour sets and 4 colour files; colour set switching fixed on HDMI; buffers cleaned up before a colour change
- gray lines between the pixels (the LCD grid), 2x and 3x: VGA - vertical, horizontal or both, HDMI - horizontal; Gray level 0..3
- TV-SOFT: Colors YES / NO (a black and white picture without the colour subcarrier)
- TV-SOFT: fixes for RP2040 (less SRAM) and for the timing around HALT
- HDMI: 504 MHz overclock option; smoother 3x picture
- VGA: selector and OSD colour fixes
- sprites: at most 10 per line, as on the Game Boy PPU; sprite order fix
- audio: sound fixes, I2S buffer without samples, audio re-initialisation on mode change
- Demo mode: no-loop option
- performance optimisation; RP2040 builds use less SRAM
- new boards: Olimex RP2040-PICO-PC with Pico 2 (PCp2), Waveshare RP2350-PiZero (z0p2); MURMULATOR 2.0 (m2p2) fixes; PSRAM improvements
- build-all.sh / build-all-rp2350.bat: every supported RP2350 configuration; the HDMI / I2S defaults of CMakeLists.txt can be overridden on the command line
- Pico SDK 2.2.0
- README rewritten for the current version

# v3.1.3
- input recording and playback by emulated frame: R / P in the ROM browser, /.config/gbc/replay/*.gbr; a save state made while recording continues the recording along a new branch when loaded
- the recording time on the bottom OSD line

# v3.1.2
- Demo mode: Start Demo (or B in the browser) plays the ROMs of a directory one after another; Demo time, Demo name OSD, Demo timer OSD
- F10: the ROM browser during a game (F10 again returns to it)
- the bottom OSD line on VGA, HDMI and TV-SOFT

# v3.1.1
- F1..F3: a new random palette for BG / OBJ0 / OBJ1, F6..F9, F12 as before
- 16 shades: 16-colour presets for Game Boy games

# v3.1.0
- LCD ghosting: a Game Boy frame is mixed with the previous one, so sprites drawn every second frame are half-transparent as on the real screen
- experimental hardware AY-3-8910 / TurboSound sound (MURMULATOR 1.x, -DHWAY=ON)

# v3.0.8
- Game Boy games: separate palettes for the background and both sprite palettes (AUTO, the GBC boot palettes, the pico-gamate palettes, Random, Custom), a palette editor, F-key shortcuts, per-game colour presets
- the settings, save states and game palettes moved to /.config/gbc (per board and video output); /GB/gb.conf is read once
- backplane: a Game Boy Color case around the game in 2x and 3x (HDMI, VGA, TV-SOFT on RP2350), replaceable by a .gbbp file from the ROM browser; tools/make_gbbp.py makes one from a picture
- VGA / TFT: the text mode is cleared on the way to the menu

# v3.0.6
- screen size 2x / 3x / Full on HDMI, VGA and TV-SOFT
- triple buffering: whole frames are shown, Game Boy Color palettes per frame, hi-colour pictures in a colour cube

# v3.0.5
- QSPI PSRAM: a ROM goes there when the RP2350 module has PSRAM, otherwise to flash
- flash: size from the JEDEC ID, sectors with the same data are not rewritten, every written sector is verified
- file browser: PageUp/PageDown/Home/End, the last ROM (or the directory left) is under the cursor on return
- keys held while the mode changes (game, menu, browser) are ignored until released
- Ctrl+Alt+Del: PSRAM released, clock and voltage lowered before the reset
- TV-SOFT: PAL/NTSC as a whole timing set, square pixels, text width by TV system
- the version is set once in CMakeLists.txt and goes into the file name

# v3.0.1
- Software TV composite output instead of hardware ntsc/pal encoder

# v3.0.0
- VGA / HDMI / TFT / TV Composite outputs added
- new filebrowser
- speed improvements and optimizations
- quality I2S sound output via TDA1387 or PCM5102 added

Toxic palette fix

# v2.2.0a

Toxic palette fix

# v2.2.0

Added ability to save and load gamestate. Better menu system.


# v2.1.0

Various overclocking variants

# v2.0.1

GameBoy Color support, overall speed increase

# v1.0.0

Huge speedup, ingame menu on SELECT+START added

# v0.7.0

Speedup and cosmetics

# v0.6.3

Double keypress in menu fix

# v0.6.2

ROM loading bugfix


# v0.6.1

Fast load previous rom after reboot, prevents PICO flash wearoff also
