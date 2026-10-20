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
