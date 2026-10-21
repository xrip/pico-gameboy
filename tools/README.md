# pico-gameboy tools

[Русская версия](README.ru.md)

## make_gbbp.py — backplane maker

Turns any picture into a backplane file (`.gbbp`) for pico-gameboy.

### Requirements

Python 3, standard library only. PNG (all colour types and bit depths, interlaced too), GIF (first frame), BMP (1/4/8/16/24/32 bits, uncompressed) and PNM (PBM/PGM/PPM) are decoded by the script itself. Other formats (JPEG, WebP, ...) are read through Pillow if it is installed (`pip install pillow`).

### Usage

```
python3 make_gbbp.py picture.png [options]
```

| Option | Default | Meaning |
|---|---|---|
| `-o`, `--output FILE` | picture name with `.gbbp` | output file |
| `--scale-x K` | 1 | cells per picture pixel, horizontally |
| `--scale-y K` | 1 | cells per picture pixel, vertically |
| `--shift-x N` | 0 | left edge of the TV-SOFT part in the scaled picture |
| `--shift-y N` | 0 | top edge in the scaled picture |
| `--scale-x3`, `--scale-y3` | computed | override the 3x scale |
| `--shift-x3`, `--shift-y3` | computed | override the 3x shift |
| `--dither` | off | Floyd–Steinberg dithering instead of the nearest colour |
| `--fill RRGGBB` | `000000` | colour outside the picture and under its transparent parts |
| `--preview FILE.png` | — | PNG with all four backplanes, game window shown in grey |

### Geometry

The backplane is made of cells. One cell is 2x2 pixels of the 640x480 screen; on TV one field line is one row of cells. At scale 1 one picture pixel maps to one cell.

- **TV-SOFT**: 384x240 cells, cut from the scaled picture at (`shift-x`, `shift-y`).
- **HDMI/VGA**: 320x240 cells, cut from the same place but 32 cells further right, so the picture sits at the same spot on the screen in both. TV-SOFT shift 0:0 means HDMI/VGA starts at 32:0 by itself.

The shift is given in coordinates of the scaled picture, i.e. it is applied after the scale.

Each part comes in two variants, one per game window:

| Screen | Game window (cells): x, y, w, h |
|---|---|
| 2x | centred, 48, 160, 144 |
| 3x | centred, 12, 240, 216 |

`--scale-*` and `--shift-*` set the 2x variant. The 3x variant is computed from them. Its window is 1.5 times bigger, so the picture is scaled 1.5 times more around the window centre (cell 192,120 of the TV-SOFT part), and what was under the 2x window stays under the 3x one:

```
scale-x3 = 1.5 * scale-x
scale-y3 = 1.5 * scale-y
shift-x3 = (shift-x + 192) * 1.5 - 192
shift-y3 = (shift-y + 120) * 1.5 - 120
```

The computed values are printed. The `--*3` options override them, and the console then shows both values, e.g. `3x shift-y3: computed 150.61, override 152 applied`.

The scale can differ by axis, for example to make the LCD area of the picture exactly match the game window.

### Colours

Each part (HDMI/VGA, TV-SOFT) gets 32 colours, chosen by k-means over both of its variants. Every cell takes the nearest of them. `--dither` spreads the error instead, which costs some noise on smooth gradients.

### Examples

```
# picture as is, top left corner at TV-SOFT 0:0
python3 make_gbbp.py picture.png

# picture of a console: the LCD bevel fits the 2x window exactly, with a preview
python3 make_gbbp.py green.png --scale-x 0.51282 --scale-y 0.53933 --shift-x 39.282 --shift-y 60.404 --preview p.png

# the same with white instead of black outside the picture
python3 make_gbbp.py green.png --scale-x 0.51282 --scale-y 0.53933 --shift-x 39.282 --shift-y 60.404 --fill ffffff -o green-white.gbbp
```

Check the result with `--preview`. It shows, top to bottom: HDMI/VGA 2x, HDMI/VGA 3x, TV-SOFT 2x, TV-SOFT 3x.

### Installing on the console

1. Copy the `.gbbp` to the SD card (ready-made ones are in `sd-card/GB`).
2. Select it in the emulator's file manager. The message is `Backplan is installed`, or `This backplan version is unsupported` if the file has a different version or size.
3. The file is copied to `/.config/gbc/backplane.gbbp` and is used from then on instead of the built-in backplane. Delete that file to get the built-in one back.

The backplane is switched in the menu with `Backplane: ON/OFF`. It is shown at `Screen: 2x` and `3x`; in `Full` it does not apply.

### File format (version 1, 338117 bytes)

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | `GBBP` |
| 4 | 1 | version (1) |
| 5 | 96 | HDMI/VGA palette: 32 x (R, G, B) |
| 101 | 76800 | HDMI/VGA 2x: 320x240 colour numbers, row by row |
| 76901 | 76800 | HDMI/VGA 3x |
| 153701 | 96 | TV-SOFT palette |
| 153797 | 92160 | TV-SOFT 2x: 384x240 |
| 245957 | 92160 | TV-SOFT 3x |

The firmware refuses a file whose size or version differs.
