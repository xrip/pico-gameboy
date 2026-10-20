#include "graphics.h"
#include <string.h>

void draw_text(const char *string, uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
    const int cols = TEXTMODE_COLS; // run-time value on TV-SOFT
    if ((int) x >= cols) return;
    uint8_t* t_buf = text_buffer + cols * 2 * y + 2 * x;
    // clip at the end of the row: the browser help line is wider than the
    // text mode on several drivers and would run into the next row
    for (int xi = cols - (int) x; xi--;) {
        if (!*string) break;
        *t_buf++ = *string++;
        *t_buf++ = bgcolor << 4 | color & 0xF;
    }
}

void draw_window(const char *title, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    char line[width + 1];
    memset(line, 0, sizeof line);
    width--;
    height--;
    // Рисуем рамки

    memset(line, 0xCD, width); // ═══


    line[0] = 0xC9; // ╔
    line[width] = 0xBB; // ╗
    draw_text(line, x, y, 11, 1);

    line[0] = 0xC8; // ╚
    line[width] = 0xBC; //  ╝
    draw_text(line, x, height + y, 11, 1);

    memset(line, ' ', width);
    line[0] = line[width] = 0xBA;

    for (int i = 1; i < height; i++) {
        draw_text(line, x, y + i, 11, 1);
    }

    snprintf(line, width - 1, " %s ", title);
    draw_text(line, x + (width - strlen(line)) / 2, y, 14, 3);
}
