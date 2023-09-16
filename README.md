# Raspberry Pi Pico GameBoy Emulator
Raspberry Pi Pico GameBoy Emulator based on [Pico-GB](https://github.com/YouMakeTech/Pico-GB) for [MURMULATOR](https://github.com/AlexEkb4ever/MURMULATOR_classical_scheme) devboard 

Murmulator devboard have MicroSD card slot, PS/2 keyboard input and VGA output

# Controls
* UP, DOWN, LEFT, RIGHT - Arrow keys
* A Button - Z key
* B Button - X key
* START - Enter key
* SELECT - Backspace key

To reboot emulator to menu press HOME key on keyboard or select+start+a+b on gamepad.
You can change game pallete by pressing Backspace+Left or Backspace+Right keys
Backspace+Up/Down on keyboard or Select+Up/Down on gamepad- change screen scaling (2x2, 3x3, 4x3)

# Hardware needed
To get it working you should have an Murmulator (development) board with VGA output. Schematics available here at https://github.com/AlexEkb4ever/MURMULATOR_classical_scheme
![Murmulator Schematics](https://github.com/javavi/pico-infonesPlus/blob/main/assets/Murmulator-1_BSchem.JPG)

# Flashing the firmware
* Download gameboy.uf2 from the [releases page](https://github.com/xrip/pico-gameboy/releases)
* Push and hold the BOOTSEL button on the Pico, then connect to your computer using a micro USB cable. Release BOOTSEL once the drive RPI-RP2 appears on your computer.
* Drag and drop the UF2 file on to the RPI-RP2 drive. The Raspberry Pi Pico will reboot and will now run the emulator.

# Preparing the SD card
The SD card is used to store game roms and save game progress. For this project, you will need a FAT 32 formatted Micro SD card with roms you legally own. Roms must have the .gb extension.

* Insert your MicroSD card in a Windows computer and format it as FAT 32
* Copy your .gb files to the SD card root folder (subfolders are not supported at this time)
* Insert the MicroSD card into the MicroSD card slot


# Building from source
The [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) is required to build this project. Make sure you are able to compile an [example project](https://github.com/raspberrypi/pico-examples#first--examples) before continuing.
```bash
cd ~
git clone https://github.com/xrip/pico-gameboy.git
cd pico-gameboy
mkdir build
cd build
cmake ..
make -j4
```


# Known issues and limitations
* No copyrighted games are included with Pico-GB / RP2040-GB. For this project, you will need a FAT 32 formatted Micro SD card with roms you legally own. Roms must have the .gb extension.
* The RP2040-GB emulator is able to run at full speed on the Pico, at the expense of emulation accuracy. Some games may not work as expected or may not work at all. RP2040-GB is still experimental and not all features are guaranteed to work.
* RP2040-GB is only compatible with [original Game Boy DMG games](https://en.wikipedia.org/wiki/List_of_Game_Boy_games) (not compatible with Game Boy Color or Game Boy Advance games)
* Repeatedly flashing your Pico will eventually wear out the flash memory (Pico is qualified for min. 100K flash/erase cycles)
* The emulator overclocks the Pico in order to get the emulator working fast enough. Overclocking can reduce the Pico’s lifespan.
* Use this software and instructions at your own risk! I will not be responsible in any way for any damage to your Pico and/or connected peripherals caused by using this software. I also do not take responsibility in any way when damage is caused to the Pico or display due to incorrect wiring or voltages.

# License
MIT
