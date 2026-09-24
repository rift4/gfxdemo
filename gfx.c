#include "graphics/gfx.h"
#include "keyboard/keyboard.h"

/* Mode 13h (320x200x256) + tiny software rasterizer.
 * Text mode return works by saving the VGA font (plane 2 at
 * 0xA0000) before chain-4 writes clobber it, then restoring
 * Mode 3 registers + font on exit. */

#define FB ((volatile unsigned char *)0xA0000)
#define PLANE_MEM ((volatile unsigned char *)0xA0000)
#define TEXT_MEM ((volatile unsigned short *)0xB8000)

static int gfx_active = 0;

static void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Standard Mode 13h register dump. */
static const unsigned char seq13[5]  = {0x03, 0x01, 0x0F, 0x00, 0x0E};
static const unsigned char crtc13[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xA3,
    0xFF
};
static const unsigned char gc13[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};
static const unsigned char ac13[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Standard text Mode 3 (80x25) register dump. */
static const unsigned char seq03[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
static const unsigned char crtc03[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};
static const unsigned char gc03[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};
static const unsigned char ac03[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* Saved VGA font (256 glyphs x 32 bytes) + text screen. */
static unsigned char font_save[256 * 32];
static unsigned short text_save[80 * 25];
static int text_saved = 0;

static void set_regs(unsigned char misc, const unsigned char *seq,
                     const unsigned char *crtc, const unsigned char *gc,
                     const unsigned char *ac)
{
    unsigned int i;
    outb(0x3C2, misc);
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);
    for (i = 1; i < 5; i++) {
        outb(0x3C4, (unsigned char)i);
        outb(0x3C5, seq[i]);
    }
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);
    outb(0x3D4, 0x11);
    outb(0x3D5, (unsigned char)(inb(0x3D5) & 0x7F));
    for (i = 0; i < 25; i++) {
        outb(0x3D4, (unsigned char)i);
        outb(0x3D5, crtc[i]);
    }
    for (i = 0; i < 9; i++) {
        outb(0x3CE, (unsigned char)i);
        outb(0x3CF, gc[i]);
    }
    (void)inb(0x3DA);
    for (i = 0; i < 21; i++) {
        outb(0x3C0, (unsigned char)i);
        outb(0x3C0, ac[i]);
    }
    outb(0x3C0, 0x20);
}

/* Map plane 2 at 0xA0000 for font save/restore (text mode only). */
static void plane2_open(void)
{
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);
    outb(0x3C4, 0x02);
    outb(0x3C5, 0x04);
    outb(0x3C4, 0x04);
    outb(0x3C5, 0x07);
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);
    outb(0x3CE, 0x04);
    outb(0x3CF, 0x02);
    outb(0x3CE, 0x05);
    outb(0x3CF, 0x00);
    outb(0x3CE, 0x06);
    outb(0x3CF, 0x00);
}

static void plane2_close_text(void)
{
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);
    outb(0x3C4, 0x02);
    outb(0x3C5, 0x03);
    outb(0x3C4, 0x04);
    outb(0x3C5, 0x03);
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);
    outb(0x3CE, 0x04);
    outb(0x3CF, 0x00);
    outb(0x3CE, 0x05);
    outb(0x3CF, 0x10);
    outb(0x3CE, 0x06);
    outb(0x3CF, 0x0E);
}

static void save_text_state(void)
{
    unsigned int i;
    for (i = 0; i < 80u * 25u; i++)
        text_save[i] = TEXT_MEM[i];
    plane2_open();
    for (i = 0; i < sizeof(font_save); i++)
        font_save[i] = PLANE_MEM[i];
    plane2_close_text();
    text_saved = 1;
}

static void restore_text_state(void)
{
    unsigned int i;
    plane2_open();
    for (i = 0; i < sizeof(font_save); i++)
        PLANE_MEM[i] = font_save[i];
    plane2_close_text();
    for (i = 0; i < 80u * 25u; i++)
        TEXT_MEM[i] = text_save[i];
}

void graphics_init(void)
{
    if (!text_saved)
        save_text_state();
    set_regs(0x63, seq13, crtc13, gc13, ac13);
    gfx_active = 1;
    gfx_clear(GFX_BLACK);
}

void graphics_text(void)
{
    set_regs(0x67, seq03, crtc03, gc03, ac03);
    if (text_saved)
        restore_text_state();
    gfx_active = 0;
}

int graphics_active(void)
{
    return gfx_active;
}

void gfx_clear(unsigned char color)
{
    unsigned int i;
    for (i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++)
        FB[i] = color;
}

void gfx_putpixel(int x, int y, unsigned char color)
{
    if (!gfx_active)
        return;
    if (x < 0 || y < 0 || x >= GFX_WIDTH || y >= GFX_HEIGHT)
        return;
    FB[(unsigned int)y * GFX_WIDTH + (unsigned int)x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned char color)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            gfx_putpixel(i, j, color);
}

void gfx_rect(int x, int y, int w, int h, unsigned char color)
{
    int i;
    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < w; i++) {
        gfx_putpixel(x + i, y, color);
        gfx_putpixel(x + i, y + h - 1, color);
    }
    for (i = 0; i < h; i++) {
        gfx_putpixel(x, y + i, color);
        gfx_putpixel(x + w - 1, y + i, color);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, unsigned char color)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;

    for (;;) {
        gfx_putpixel(x, y, color);
        if (x == x1 && y == y1)
            break;
        {
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x += sx;
            }
            if (e2 < dx) {
                err += dx;
                y += sy;
            }
        }
    }
}

/* 5x7 glyphs, 7 rows of 5 bits (bit4 = left pixel). */
static const unsigned char font_space[7] = {0, 0, 0, 0, 0, 0, 0};

struct glyph {
    char c;
    const unsigned char *rows;
};

#define G(ch, a, b, cc, d, e, f, g) \
    static const unsigned char font_##ch[7] = {a, b, cc, d, e, f, g}

G(A, 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11);
G(B, 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E);
G(C, 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E);
G(D, 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C);
G(E, 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F);
G(F, 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10);
G(G, 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F);
G(H, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11);
G(I, 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E);
G(J, 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C);
G(K, 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11);
G(L, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F);
G(M, 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11);
G(N, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11);
G(O, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E);
G(P, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10);
G(Q, 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D);
G(R, 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11);
G(S, 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E);
G(T, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04);
G(U, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E);
G(V, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04);
G(W, 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11);
G(X, 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11);
G(Y, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04);
G(Z, 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F);
G(n0, 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E);
G(n1, 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E);
G(n2, 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F);
G(n3, 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E);
G(n4, 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02);
G(n5, 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E);
G(n6, 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E);
G(n7, 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08);
G(n8, 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E);
G(n9, 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C);
G(MINUS, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00);
G(XCHAR, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00);

static const unsigned char *font_lookup(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    switch (c) {
    case ' ': return font_space;
    case 'A': return font_A;
    case 'B': return font_B;
    case 'C': return font_C;
    case 'D': return font_D;
    case 'E': return font_E;
    case 'F': return font_F;
    case 'G': return font_G;
    case 'H': return font_H;
    case 'I': return font_I;
    case 'J': return font_J;
    case 'K': return font_K;
    case 'L': return font_L;
    case 'M': return font_M;
    case 'N': return font_N;
    case 'O': return font_O;
    case 'P': return font_P;
    case 'Q': return font_Q;
    case 'R': return font_R;
    case 'S': return font_S;
    case 'T': return font_T;
    case 'U': return font_U;
    case 'V': return font_V;
    case 'W': return font_W;
    case 'X': return font_X;
    case 'Y': return font_Y;
    case 'Z': return font_Z;
    case '0': return font_n0;
    case '1': return font_n1;
    case '2': return font_n2;
    case '3': return font_n3;
    case '4': return font_n4;
    case '5': return font_n5;
    case '6': return font_n6;
    case '7': return font_n7;
    case '8': return font_n8;
    case '9': return font_n9;
    case '-': return font_MINUS;
    default: return font_XCHAR;
    }
}

void gfx_draw_char(int x, int y, char c, unsigned char color, int scale)
{
    const unsigned char *rows = font_lookup(c);
    int r, col, sx, sy;
    if (scale < 1)
        scale = 1;
    for (r = 0; r < 7; r++)
        for (col = 0; col < 5; col++)
            if (rows[r] & (1u << (4 - col)))
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        gfx_putpixel(x + col * scale + sx,
                                     y + r * scale + sy, color);
}

void gfx_draw_text(int x, int y, const char *s, unsigned char color, int scale)
{
    int cx = x;
    if (scale < 1)
        scale = 1;
    while (*s) {
        gfx_draw_char(cx, y, *s, color, scale);
        cx += 6 * scale;
        s++;
    }
}

void gfx_demo(void)
{
    int i;

    if (!gfx_active)
        graphics_init();

    gfx_clear(GFX_BLACK);

    /* Outer frame. */
    gfx_rect(10, 10, 300, 180, GFX_WHITE);
    gfx_rect(12, 12, 296, 176, GFX_DARK_GREY);

    /* Title, centered: 6 chars * 6px * 3 = 108px wide. */
    gfx_draw_text(106, 26, "TINYOS", GFX_YELLOW, 3);

    /* Subtitle. */
    gfx_draw_text(98, 54, "320X200 MODE 13H", GFX_LIGHT_GREY, 1);

    /* The box from the sketch: solid top bar + hollow body. */
    gfx_fill_rect(100, 80, 120, 60, GFX_BLUE);
    gfx_fill_rect(100, 80, 120, 12, GFX_WHITE);
    gfx_rect(100, 80, 120, 60, GFX_WHITE);
    gfx_line(100, 80, 219, 139, GFX_DARK_GREY);
    gfx_draw_text(127, 102, "TINYOS", GFX_WHITE, 2);

    /* Palette strip along the bottom. */
    for (i = 0; i < 16; i++)
        gfx_fill_rect(36 + i * 16, 152, 16, 16, (unsigned char)i);
    gfx_rect(36, 152, 256, 16, GFX_WHITE);

    /* Corner accents. */
    gfx_line(10, 10, 30, 30, GFX_LIGHT_RED);
    gfx_line(309, 10, 289, 30, GFX_LIGHT_RED);
    gfx_line(10, 189, 30, 169, GFX_LIGHT_RED);
    gfx_line(309, 189, 289, 169, GFX_LIGHT_RED);

    gfx_draw_text(88, 176, "PRESS ANY KEY", GFX_WHITE, 1);

    (void)keyboard_getc();
    graphics_text();
}
