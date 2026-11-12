#!/bin/sh
# Build every supported RP2350 configuration of pico-gameboy (Linux, macOS,
# Git Bash).  The Pico SDK comes from PICO_SDK_PATH, as for a normal build.
# Excluded: RP2040, RISC-V, m1p2launcher, TFT/ILI9341, the old TV driver.
# The UF2 files land in bin/rp2350/<BUILD_TYPE>/.
#
#   ./build-all.sh                 all variants
#   ./build-all.sh murmulator      the variants of one board
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_TYPE=${BUILD_TYPE:-Release}
BUILD_ROOT=${BUILD_ROOT:-$ROOT/build/all}
if command -v ninja >/dev/null 2>&1; then GENERATOR=Ninja; else GENERATOR="Unix Makefiles"; fi
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# board: video outputs: sound outputs (as wired on the board)
VARIANTS="
murmulator:VGA HDMI SOFTTV:PWM I2S HWAY
murmulator2:VGA HDMI:PWM I2S
olimex-pico-pc:HDMI:PWM
waveshare_rp2350_pizero:HDMI:I2S
"

build_one() {
    board=$1 video=$2 sound=$3
    tag=$board-$video-$sound
    vga=OFF hdmi=OFF softtv=OFF i2s=OFF hway=OFF
    case $video in VGA) vga=ON ;; HDMI) hdmi=ON ;; SOFTTV) softtv=ON ;; esac
    case $sound in I2S) i2s=ON ;; HWAY) hway=ON ;; esac
    echo
    echo "[$tag]"
    cmake -S "$ROOT" -B "$BUILD_ROOT/$tag" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPICO_BOARD="$board" \
        -Dm1p2launcher=OFF -DTFT=OFF -DILI9341=OFF -DTV=OFF \
        -DVGA=$vga -DHDMI=$hdmi -DSOFTTV=$softtv \
        -DI2S=$i2s -DI2S_CS4334=OFF -DHWAY=$hway >/dev/null
    cmake --build "$BUILD_ROOT/$tag" --parallel "$JOBS" >/dev/null
}

count=0
echo "$VARIANTS" | while IFS=: read -r board videos sounds; do
    [ -n "$board" ] || continue
    [ -z "$1" ] || [ "$1" = "$board" ] || continue
    for video in $videos; do
        for sound in $sounds; do
            build_one "$board" "$video" "$sound"
        done
    done
done

echo
echo "UF2 files: $ROOT/bin/rp2350/$BUILD_TYPE/"
