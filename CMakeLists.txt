cmake_minimum_required(VERSION 3.13...3.23)
include(pico_sdk_import.cmake)

project(gameboy-color C CXX ASM)
SET(BUILD_NAME "${PROJECT_NAME}")

option(I2S "Enable I2S sound" OFF)
option(I2S_CS4334 "Enable I2S CS4334 sound" OFF)
option(VGA "Enable VGA" OFF)
option(TFT "Enable TFT display" OFF)
option(ILI9341 "Enable TFT ILI9341 display" OFF)
option(HDMI "Enable HDMI display" OFF)
option(TV "Enable TV composite output" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")

pico_sdk_init()

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/audio)

add_subdirectory(drivers/vga-nextgen)
add_subdirectory(drivers/hdmi)
add_subdirectory(drivers/st7789)
add_subdirectory(drivers/tv)

add_subdirectory(drivers/graphics)

add_executable(${PROJECT_NAME}
        ext/minigb_apu/minigb_apu.c
        src/main.cpp
)


pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
pico_set_boot_stage2(${PROJECT_NAME} slower_boot2)

set(FAMILY rp2040)
set(BOARD pico_sdk)

pico_set_program_name(gameboy-color "Gameboy Emulator by xrip")
pico_set_program_version(gameboy-color "test")

target_include_directories(${PROJECT_NAME} PRIVATE
        src
        inc
        ext/minigb_apu
)

IF(TFT)
    target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TFT)
    SET(BUILD_NAME "${BUILD_NAME}-TFT")
    IF(ILI9341)
        SET(BUILD_NAME "${BUILD_NAME}-ILI9341")
        target_compile_definitions(${PROJECT_NAME} PRIVATE ILI9341)
    ELSE()
        SET(BUILD_NAME "${BUILD_NAME}-ST7789")
    ENDIF()
ELSEIF(HDMI)
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI)
    SET(BUILD_NAME "${BUILD_NAME}-HDMI")
ELSEIF(TV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    SET(BUILD_NAME "${BUILD_NAME}-TV")
ELSE()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    SET(BUILD_NAME "${BUILD_NAME}-VGA")
ENDIF()

IF(NOT I2S)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_PWM_PIN=26)
ELSEIF(I2S_CS4334)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_CS4334)
    SET(BUILD_NAME "${BUILD_NAME}-I2S-CS4334")
ELSE()
    SET(BUILD_NAME "${BUILD_NAME}-I2S-TDA1387")
ENDIF()

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

target_link_libraries(${PROJECT_NAME} PRIVATE
        audio
        graphics

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

        tinyusb_board
        tinyusb_device
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
        # 16MB flash
        PICO_FLASH_SIZE_BYTES=16777216

        # SDCARD
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4

        # PS2KBD
        PS2KBD_GPIO_FIRST=0

        # NES Gamepad
        NES_GPIO_CLK=14
        NES_GPIO_DATA=16
        NES_GPIO_LAT=15

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=6

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=6

        # TFT
        TFT_CS_PIN=6
        TFT_RST_PIN=8
        TFT_LED_PIN=9
        TFT_DC_PIN=10
        TFT_DATA_PIN=12
        TFT_CLK_PIN=13
)

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)
pico_add_extra_outputs(${PROJECT_NAME})