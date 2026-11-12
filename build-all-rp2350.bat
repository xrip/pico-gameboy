@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build every supported RP2350 configuration of pico-gameboy (Windows).
rem The Pico SDK comes from PICO_SDK_PATH or the Raspberry Pi Pico VS Code
rem extension (%USERPROFILE%\.pico-sdk), as for a normal build.
rem Excluded: RP2040, RISC-V, m1p2launcher, TFT/ILI9341, the old TV driver.
rem The UF2 files land in bin\rp2350\Release\.

set "ROOT=%~dp0"
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
cd /d "%ROOT%"

set "BUILD_TYPE=Release"
set "BUILD_ROOT=%ROOT%build\all"

rem Locate CMake.
set "CMAKE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
if not defined CMAKE if exist "%USERPROFILE%\.pico-sdk" (
    for /f "delims=" %%I in ('where /r "%USERPROFILE%\.pico-sdk" cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
)
if not defined CMAKE (
    echo ERROR: cmake.exe not found. 1>&2
    exit /b 1
)

rem Locate Ninja.
set "NINJA="
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
if not defined NINJA if exist "%USERPROFILE%\.pico-sdk" (
    for /f "delims=" %%I in ('where /r "%USERPROFILE%\.pico-sdk" ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
)
if not defined NINJA (
    echo ERROR: ninja.exe not found in PATH or "%USERPROFILE%\.pico-sdk". 1>&2
    exit /b 1
)

echo CMake: %CMAKE%
echo Ninja: %NINJA%
echo Source: %SRC%

set /a COUNT=0
set "TOTAL=15"

rem board, video outputs, sound outputs - as wired on the board
for %%V in (VGA HDMI SOFTTV) do for %%A in (PWM I2S HWAY) do (
    call :build_one murmulator %%V %%A || exit /b 1
)
for %%V in (VGA HDMI) do for %%A in (PWM I2S) do (
    call :build_one murmulator2 %%V %%A || exit /b 1
)
call :build_one olimex-pico-pc HDMI PWM || exit /b 1
call :build_one waveshare_rp2350_pizero HDMI I2S || exit /b 1

echo.
echo All %TOTAL% variants built.
echo UF2 files are under bin\rp2350\%BUILD_TYPE%\.
exit /b 0

:build_one
set /a COUNT+=1
set "B=%~1"
set "V=%~2"
set "A=%~3"
set "TAG=!B!-!V!-!A!"
set "BDIR=%BUILD_ROOT%\!TAG!"

set "VGA=OFF"
set "HDMI=OFF"
set "SOFTTV=OFF"
if /I "!V!"=="VGA" set "VGA=ON"
if /I "!V!"=="HDMI" set "HDMI=ON"
if /I "!V!"=="SOFTTV" set "SOFTTV=ON"

set "I2S=OFF"
set "HWAY=OFF"
if /I "!A!"=="I2S" set "I2S=ON"
if /I "!A!"=="HWAY" set "HWAY=ON"

echo.
echo [!COUNT!/%TOTAL%] !TAG!

"%CMAKE%" -S "%SRC%" -B "!BDIR!" -G Ninja ^
  "-DCMAKE_MAKE_PROGRAM:FILEPATH=%NINJA%" ^
  "-DCMAKE_BUILD_TYPE=%BUILD_TYPE%" ^
  "-DPICO_BOARD=!B!" ^
  "-Dm1p2launcher=OFF" ^
  "-DTFT=OFF" ^
  "-DILI9341=OFF" ^
  "-DTV=OFF" ^
  "-DVGA=!VGA!" ^
  "-DHDMI=!HDMI!" ^
  "-DSOFTTV=!SOFTTV!" ^
  "-DI2S=!I2S!" ^
  "-DI2S_CS4334=OFF" ^
  "-DHWAY=!HWAY!"
if errorlevel 1 (
    echo ERROR: configure failed for !TAG!. 1>&2
    exit /b 1
)

"%CMAKE%" --build "!BDIR!" --config %BUILD_TYPE%
if errorlevel 1 (
    echo ERROR: build failed for !TAG!. 1>&2
    exit /b 1
)
exit /b 0
