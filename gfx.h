#ifndef GFX_H
#define GFX_H

/* Tiny Mode 13h graphics subsystem: 320x200, 256 colors. */

#define GFX_WIDTH 320
#define GFX_HEIGHT 200

/* First 16 DAC entries match the classic text palette. */
#define GFX_BLACK 0
#define GFX_BLUE 1
#define GFX_GREEN 2
#define GFX_CYAN 3
#define GFX_RED 4
#define GFX_MAGENTA 5
#define GFX_BROWN 6
#define GFX_LIGHT_GREY 7
#define GFX_DARK_GREY 8
#define GFX_LIGHT_BLUE 9
#define GFX_LIGHT_GREEN 10
#define GFX_LIGHT_CYAN 11
#define GFX_LIGHT_RED 12
#define GFX_LIGHT_MAGENTA 13
#define GFX_YELLOW 14
#define GFX_WHITE 15

void graphics_init(void);
void graphics_text(void);
int graphics_active(void);

void gfx_clear(unsigned char color);
void gfx_putpixel(int x, int y, unsigned char color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned char color);
void gfx_rect(int x, int y, int w, int h, unsigned char color);
void gfx_line(int x0, int y0, int x1, int y1, unsigned char color);

/* 5x7 block font for A-Z, 0-9 and a few symbols (scale 1..N). */
void gfx_draw_char(int x, int y, char c, unsigned char color, int scale);
void gfx_draw_text(int x, int y, const char *s, unsigned char color, int scale);

/* Demo matching the README sketch: border + TINYOS box. */
void gfx_demo(void);

#endif
