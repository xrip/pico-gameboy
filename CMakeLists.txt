cmake_minimum_required(VERSION 3.13...3.23)

include(pico_sdk_import.cmake)

project(gameboy C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/vga)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/audio)

add_executable(gameboy
        src/main.cpp
        ext/minigb_apu/minigb_apu.c
)

target_include_directories(gameboy PRIVATE inc ext/minigb_apu)

target_link_libraries(gameboy
        audio

        vga
        ps2kbd
        sdcard
        fatfs
        nespad


        pico_mem_ops
        pico_stdlib
        pico_stdio
        pico_bootrom
        pico_multicore
        pico_stdio
        pico_multicore

        hardware_clocks hardware_pio hardware_vreg hardware_pio hardware_pwm
        hardware_sync hardware_pll hardware_spi hardware_irq hardware_dma
        pico_binary_info
)

target_compile_definitions(gameboy PRIVATE
        PARAM_ASSERTIONS_DISABLE_ALL=1
        PICO_ENTER_USB_BOOT_ON_EXIT=1
        PICO_STDIO_ENABLE_CRLF_SUPPORT=0
        PICO_STDIO_DEFAULT_CRLF=0
        PICO_PRINTF_SUPPORT_FLOAT=0
        PICO_PRINTF_SUPPORT_EXPONENTIAL=0
        PICO_PRINTF_SUPPORT_LONG_LONG=1
        PICO_PRINTF_SUPPORT_PTRDIFF_T=0

        # VGA
        VGA_GPIO_FIRST=6
        VGA_GPIO_OUTNUM=6
        VGA_GPIO_SYNC=12
        VGA_VSYNC=13

        # SDCARD
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4

        # PS2KBD
        PS2KBD_GPIO_FIRST=0

        # NES Gamepad
        NES_GPIO_CLK=14
        NES_GPIO_LAT=15
        NES_GPIO_DATA=16

        # AUDIO
        AUDIO_PWM_PIN=26

        # Other
        FF_USE_FIND
)


function(pico_add_verbose_dis_output TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_OBJDUMP} -h $<TARGET_FILE:${TARGET}> >$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
            COMMAND ${CMAKE_OBJDUMP} -drwCSl $<TARGET_FILE:${TARGET}> >>$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
    )
endfunction()

pico_set_program_name(gameboy "GameBoy for MURMULATOR devboard")
pico_set_program_version(gameboy "v0.1")

#pico_set_binary_type(gameboy copy_to_ram)
#pico_set_binary_type(gameboy no_flash)
pico_enable_stdio_usb(gameboy 1)
pico_enable_stdio_uart(gameboy 0)
pico_add_verbose_dis_output(gameboy)
pico_add_bin_output(gameboy)
pico_add_uf2_output(gameboy)
