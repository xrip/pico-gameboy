cmake_minimum_required(VERSION 3.13...3.23)

include(pico_sdk_import.cmake)

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
project(gameboy-color C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/vga)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/audio)

add_executable(${PROJECT_NAME}
        ext/minigb_apu/minigb_apu.c
        src/main.cpp
)

if(OVERCLOCKING)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME  "${PROJECT_NAME}-${OVERCLOCKING}MHz")

    target_compile_definitions(${PROJECT_NAME} PRIVATE OVERCLOCKING=${OVERCLOCKING})

    pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
    target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
    pico_set_boot_stage2(${PROJECT_NAME} slower_boot2)
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE inc ext/minigb_apu)

target_link_libraries(${PROJECT_NAME}
        audio

        vga
        ps2kbd
        sdcard
        fatfs
        nespad

        pico_runtime
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

target_compile_definitions(${PROJECT_NAME} PRIVATE
        # 16MB flash
        PICO_FLASH_SIZE_BYTES=16777216

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

pico_set_program_name(${PROJECT_NAME} "GameBoy for MURMULATOR devboard")
pico_set_program_version(${PROJECT_NAME} "v0.1")

#pico_set_binary_type(${PROJECT_NAME} copy_to_ram)
#pico_set_binary_type(${PROJECT_NAME} no_flash)
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)
#pico_add_verbose_dis_output(${PROJECT_NAME})
#pico_add_bin_output(${PROJECT_NAME})
pico_add_uf2_output(${PROJECT_NAME})
