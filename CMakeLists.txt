# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13...3.23)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards )
set(PICO_PLATFORM "rp2350" CACHE STRING "Chip type")
#set(PICO_PLATFORM "rp2040" CACHE STRING "Chip type")
#set(PICO_BOARD "murmulator" CACHE STRING "Board type")
set(PICO_BOARD "murmulator2" CACHE STRING "Board type")
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
option(SOFTTV "Enable TV soft composite output" OFF)
if( ${PICO_PLATFORM} MATCHES "rp2350" )
option(m1p2launcher "Enable m1p2-launcher support" OFF)
endif()

#set(m1p2launcher ON)
#set(I2S ON)
#set(TFT ON)
#set(ILI9341 ON)
#set(HDMI ON)
#set(TV ON)
#set(SOFTTV ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}  -funroll-loops -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -funroll-loops -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")

pico_sdk_init()

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${PICO_PLATFORM}/${CMAKE_BUILD_TYPE}")
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
add_subdirectory(drivers/tv-software)

add_subdirectory(drivers/graphics)

add_executable(${PROJECT_NAME}
        ext/minigb_apu/minigb_apu.c
        src/main.cpp
)


if( ${PICO_PLATFORM} MATCHES "rp2040" )
	pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
	target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
	pico_set_boot_stage2(${PROJECT_NAME} slower_boot2)
	if ( ${PICO_BOARD} MATCHES "murmulator2")
		SET(BUILD_NAME "m2p1-${PROJECT_NAME}")
	else()
		SET(BUILD_NAME "m1p1-${PROJECT_NAME}")
	endif()
else()
    if (m1p2launcher)
		pico_set_linker_script(${PROJECT_NAME} "${CMAKE_SOURCE_DIR}/memmap.ld")
	endif()
	if ( ${PICO_BOARD} MATCHES "murmulator2")
		SET(BUILD_NAME "m2p2-${PROJECT_NAME}")
	else()
		SET(BUILD_NAME "m1p2-${PROJECT_NAME}")
	endif()
endif()

if ( ${PICO_BOARD} MATCHES "murmulator2")
	pico_set_program_name(gameboy-color "Gameboy emulator by xrip for MURMULATOR 2.0 board")
else()
	pico_set_program_name(gameboy-color "Gameboy emulator by xrip for MURMULATOR 1.x board")
endif()
pico_set_program_version(gameboy-color "3.0.3")

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
ELSEIF(SOFTTV)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTTV)
	target_link_libraries(${PROJECT_NAME} PRIVATE tv-software)
	SET(BUILD_NAME "${BUILD_NAME}-TV-SOFT")
ELSE()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    SET(BUILD_NAME "${BUILD_NAME}-VGA")
ENDIF()

IF(NOT I2S)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_PWM)
    SET(BUILD_NAME "${BUILD_NAME}-PWM")
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
        hardware_pwm

        tinyusb_board
        tinyusb_device
)

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)
pico_add_extra_outputs(${PROJECT_NAME})
