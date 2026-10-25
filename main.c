/*
 * TETRIS // HACK  -  тетрис в стиле киберпанка для PSP (homebrew, ARK-4)
 *
 * Без внешних библиотек: рисуем программно в RAM-буфер 480x272 (ABGR8888)
 * и копируем его в VRAM после vblank.
 *
 * Особенности: вращение SRS с «пинками» от стен, мешок из 7 фигур,
 * удержание (hold), тень (ghost), 3 следующие фигуры, задержка фиксации,
 * автоповтор (DAS/ARR), комбо, уровни, рекорд в файле.
 *
 * Управление:
 *   D-Pad влево/вправо ..... движение (с автоповтором)
 *   D-Pad вниз ............. мягкое падение
 *   D-Pad вверх ............ мгновенное падение
 *   X или КРУГ ............. поворот по часовой
 *   КВАДРАТ ................ поворот против часовой
 *   L, R или ТРЕУГОЛЬНИК ... удержать фигуру (hold)
 *   START .................. старт / пауза
 *   SELECT ................. начать заново
 *   HOME ................... выход
 *   (аналог дублирует влево / вправо / вниз)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef HOST_TEST
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>

PSP_MODULE_INFO("TetrisHack", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif

typedef uint32_t u32;

/* ------------------------------------------------------------------ */
/* Константы                                                           */
/* ------------------------------------------------------------------ */
#define SCR_W 480
#define SCR_H 272
#define BUF_W 512            /* stride фреймбуфера PSP */

#define COLS  10
#define ROWS  22             /* 2 верхние строки скрыты */
#define HID   2
#define VIS   (ROWS - HID)   /* 20 видимых строк */
#define CELL  13
#define BW    (COLS * CELL)  /* 130 px */
#define BH    (VIS * CELL)   /* 260 px */
#define BX    175            /* позиция поля на экране */
#define BY    6

#define DAS_MS    170.0      /* задержка автоповтора */
#define ARR_MS    40.0       /* период автоповтора */
#define LOCK_MS   500.0      /* задержка фиксации */
#define CLEAR_MS  380.0      /* длительность анимации линий */

#define RGB(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

#define C_YELLOW  RGB(252, 238, 10)
#define C_YELLOW2 RGB(185, 174, 0)
#define C_CYAN    RGB(0, 240, 255)
#define C_MAGENTA RGB(255, 42, 109)
#define C_TEXT    RGB(236, 235, 245)
#define C_MUTED   RGB(140, 139, 168)
#define C_BLACK   RGB(0, 0, 0)
#define C_WHITE   RGB(255, 255, 255)

static const u32 PCOL[7] = {
    RGB(0, 240, 255),    /* I - циан */
    RGB(252, 238, 10),   /* O - жёлтый */
    RGB(176, 70, 255),   /* T - фиолетовый */
    RGB(57, 255, 120),   /* S - зелёный */
    RGB(255, 42, 109),   /* Z - маджента */
    RGB(60, 120, 255),   /* J - синий */
    RGB(255, 150, 30),   /* L - оранжевый */
};

/* ------------------------------------------------------------------ */
/* Буферы и примитивы рисования                                        */
/* ------------------------------------------------------------------ */
static u32 frame[SCR_H * BUF_W];     /* текущий кадр */
static u32 bg[SCR_H * BUF_W];        /* статичный фон (рисуется один раз) */
static u32 boardbg[BW * BH];         /* фон поля: сетка + виньетка */
static u32 *T = frame;               /* целевой буфер рисования */

static u32 rng = 2463534242u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float rndf(void) { return (rnd() & 0xFFFFFF) / 16777216.0f; }

/* смешивание: a = 0..256 */
static inline u32 mix(u32 d, u32 s, int a)
{
    int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
    int sr = s & 255, sg = (s >> 8) & 255, sb = (s >> 16) & 255;
    dr += ((sr - dr) * a) >> 8;
    dg += ((sg - dg) * a) >> 8;
    db += ((sb - db) * a) >> 8;
    return RGB(dr, dg, db);
}

static void rect(int x, int y, int w, int h, u32 c, int a)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        u32 *p = T + (y + j) * BUF_W + x;
        if (a >= 256) for (i = 0; i < w; i++) p[i] = c;
        else          for (i = 0; i < w; i++) p[i] = mix(p[i], c, a);
    }
}
#define fill(x, y, w, h, c)      rect(x, y, w, h, c, 256)
#define tint(x, y, w, h, c, a)   rect(x, y, w, h, c, a)

static void outline(int x, int y, int w, int h, u32 c, int a)
{
    rect(x, y, w, 1, c, a);
    rect(x, y + h - 1, w, 1, c, a);
    rect(x, y + 1, 1, h - 2, c, a);
    rect(x + w - 1, y + 1, 1, h - 2, c, a);
}

/* прямоугольник в координатах поля с обрезкой по полю */
static void brect(int x, int y, int w, int h, u32 c, int a)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BW) w = BW - x;
    if (y + h > BH) h = BH - y;
    if (w <= 0 || h <= 0) return;
    rect(BX + x, BY + y, w, h, c, a);
}

/* ------------------------------------------------------------------ */
/* Шрифт 5x7 (ASCII 32..90, только заглавные)                          */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[59][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x56,0x20,0x50}, /* & */
    {0x00,0x08,0x07,0x03,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x2A,0x1C,0x7F,0x1C,0x2A}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x80,0x70,0x30,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x00,0x60,0x60,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x72,0x49,0x49,0x49,0x46}, /* 2 */
    {0x21,0x41,0x49,0x4D,0x33}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x31}, /* 6 */
    {0x41,0x21,0x11,0x09,0x07}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x46,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x00,0x14,0x00,0x00}, /* : */
    {0x00,0x40,0x34,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x59,0x09,0x06}, /* ? */
    {0x3E,0x41,0x5D,0x59,0x4E}, /* @ */
    {0x7C,0x12,0x11,0x12,0x7C}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x41,0x3E}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x73}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x1C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x26,0x49,0x49,0x49,0x32}, /* S */
    {0x03,0x01,0x7F,0x01,0x03}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x59,0x49,0x4D,0x43}, /* Z */
};

static void text(int x, int y, const char *s, int sc, u32 c)
{
    for (; *s; s++, x += 6 * sc) {
        int ch = (unsigned char)*s, col, row;
        const uint8_t *g;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 90) continue;
        g = font5x7[ch - 32];
        for (col = 0; col < 5; col++)
            for (row = 0; row < 7; row++)
                if ((g[col] >> row) & 1)
                    rect(x + col * sc, y + row * sc, sc, sc, c, 256);
    }
}
static int text_w(const char *s, int sc) { int n = (int)strlen(s); return n ? n * 6 * sc - sc : 0; }
static void text_c(int cx, int y, const char *s, int sc, u32 c) { text(cx - text_w(s, sc) / 2, y, s, sc, c); }

/* заголовок с хроматическим сдвигом */
static void title_c(int cx, int y, const char *s, int sc, u32 c)
{
    int sh = sc / 2 < 1 ? 1 : sc / 2;
    text_c(cx + sh, y, s, sc, C_MAGENTA);
    text_c(cx - sh, y, s, sc, C_CYAN);
    text_c(cx, y, s, sc, c);
}

/* ------------------------------------------------------------------ */
/* Блок («неоновый кубик») и превью фигуры                             */
/* ------------------------------------------------------------------ */
static void block(int x, int y, int s, u32 c)
{
    tint(x - 1, y - 1, s + 2, s + 2, c, 14);
    fill(x + 1, y + 1, s - 2, s - 2, mix(c, C_BLACK, 130));
    fill(x + 2, y + 2, s - 4, s - 4, c);
    tint(x + 2, y + 2, s - 4, 1, C_WHITE, 120);
    tint(x + 2, y + 3, 1, s - 5, C_WHITE, 80);
}

/* ------------------------------------------------------------------ */
/* Фигуры и вращение (SRS)                                             */
/* ------------------------------------------------------------------ */
static const int8_t base_shape[7][4][2] = {
    /* I */ {{0,1},{1,1},{2,1},{3,1}},
    /* O */ {{1,0},{2,0},{1,1},{2,1}},
    /* T */ {{1,0},{0,1},{1,1},{2,1}},
    /* S */ {{1,0},{2,0},{0,1},{1,1}},
    /* Z */ {{0,0},{1,0},{1,1},{2,1}},
    /* J */ {{0,0},{0,1},{1,1},{2,1}},
    /* L */ {{2,0},{0,1},{1,1},{2,1}},
};
static const int boxsz[7] = { 4, 4, 3, 3, 3, 3, 3 };
enum { P_I, P_O, P_T, P_S, P_Z, P_J, P_L };

static int8_t shape[7][4][4][2];     /* [тип][поворот][клетка][x,y] */

/* таблицы «пинков» SRS: порядок переходов 0>1,1>0,1>2,2>1,2>3,3>2,3>0,0>3;
   y направлен вверх, как в описании SRS */
static const int8_t kick_jlstz[8][5][2] = {
    {{0,0},{-1,0},{-1, 1},{0,-2},{-1,-2}},
    {{0,0},{ 1,0},{ 1,-1},{0, 2},{ 1, 2}},
    {{0,0},{ 1,0},{ 1,-1},{0, 2},{ 1, 2}},
    {{0,0},{-1,0},{-1, 1},{0,-2},{-1,-2}},
    {{0,0},{ 1,0},{ 1, 1},{0,-2},{ 1,-2}},
    {{0,0},{-1,0},{-1,-1},{0, 2},{-1, 2}},
    {{0,0},{-1,0},{-1,-1},{0, 2},{-1, 2}},
    {{0,0},{ 1,0},{ 1, 1},{0,-2},{ 1,-2}},
};
static const int8_t kick_i[8][5][2] = {
    {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1, 2}},
    {{0,0},{ 2,0},{-1,0},{ 2, 1},{-1,-2}},
    {{0,0},{-1,0},{ 2,0},{-1, 2},{ 2,-1}},
    {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2, 1}},
    {{0,0},{ 2,0},{-1,0},{ 2, 1},{-1,-2}},
    {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1, 2}},
    {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2, 1}},
    {{0,0},{-1,0},{ 2,0},{-1, 2},{ 2,-1}},
};

static void init_shapes(void)
{
    int t, r, i;
    for (t = 0; t < 7; t++) {
        int B = boxsz[t];
        for (i = 0; i < 4; i++) { shape[t][0][i][0] = base_shape[t][i][0]; shape[t][0][i][1] = base_shape[t][i][1]; }
        for (r = 1; r < 4; r++)
            for (i = 0; i < 4; i++) {
                if (t == P_O) {                     /* квадрат не вращается */
                    shape[t][r][i][0] = shape[t][0][i][0];
                    shape[t][r][i][1] = shape[t][0][i][1];
                } else {                            /* поворот по часовой: (x,y) -> (B-1-y, x) */
                    shape[t][r][i][0] = (int8_t)(B - 1 - shape[t][r - 1][i][1]);
                    shape[t][r][i][1] = shape[t][r - 1][i][0];
                }
            }
    }
}

/* превью фигуры, центрированное в точке (cx, cy) */
static void draw_preview(int type, int cx, int cy, int s, int dim)
{
    int i, minx = 9, maxx = -9, miny = 9, maxy = -9, w, h, ox, oy;
    u32 col = PCOL[type];
    if (type < 0) return;
    if (dim) col = mix(col, RGB(20, 20, 30), 170);
    for (i = 0; i < 4; i++) {
        int x = shape[type][0][i][0], y = shape[type][0][i][1];
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    w = (maxx - minx + 1) * s;
    h = (maxy - miny + 1) * s;
    ox = cx - w / 2;
    oy = cy - h / 2;
    for (i = 0; i < 4; i++)
        block(ox + (shape[type][0][i][0] - minx) * s, oy + (shape[type][0][i][1] - miny) * s, s, col);
}

/* ------------------------------------------------------------------ */
/* Статичный фон: градиенты, сетка, силуэт города, панели              */
/* ------------------------------------------------------------------ */
static void panel(int x, int y, int w, int h)
{
    tint(x, y, w, h, RGB(13, 13, 25), 195);
    outline(x, y, w, h, C_YELLOW2, 200);
    fill(x, y, 18, 2, C_YELLOW);
    fill(x + w - 18, y + h - 2, 18, 2, C_MAGENTA);
}

static void slot(int cx, int cy)       /* рамка под превью фигуры */
{
    tint(cx - 50, cy - 16, 100, 32, RGB(5, 5, 11), 200);
    outline(cx - 50, cy - 16, 100, 32, C_CYAN, 90);
}

#define LP_X 8                 /* левая панель */
#define RP_X 315               /* правая панель */
#define PN_W 157
#define PN_Y 6
#define PN_H 260
#define LCX  (LP_X + PN_W / 2)
#define RCX  (RP_X + PN_W / 2)

static void build_bg(void)
{
    int x, y, i, k, bx;
    u32 saved = rng;
    T = bg;
    rng = 0x1234ABCDu;

    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            u32 c = RGB(7, 7, 13);
            float d1 = sqrtf((float)(x * x + y * y)) / 330.0f;
            float d2 = sqrtf((float)((SCR_W - 1 - x) * (SCR_W - 1 - x) + (SCR_H - 1 - y) * (SCR_H - 1 - y))) / 330.0f;
            if (d1 < 1.0f) c = mix(c, C_MAGENTA, (int)((1.0f - d1) * 0.20f * 256));
            if (d2 < 1.0f) c = mix(c, C_CYAN,    (int)((1.0f - d2) * 0.16f * 256));
            if ((x % 16) == 0 || (y % 16) == 0) c = mix(c, C_WHITE, 5);
            bg[y * BUF_W + x] = c;
        }
    }

    /* силуэт ночного города */
    bx = 0;
    while (bx < SCR_W) {
        int w = 16 + (int)(rnd() % 26), h = 24 + (int)(rnd() % 52), nwin;
        u32 pal[3];
        pal[0] = C_YELLOW; pal[1] = C_CYAN; pal[2] = C_MAGENTA;
        fill(bx, SCR_H - h, w, h, RGB(11, 11, 23));
        tint(bx, SCR_H - h, w, 1, (rnd() & 1) ? C_MAGENTA : C_CYAN, 115);
        nwin = w * h / 140;
        for (k = 0; k < nwin; k++) {
            int wx = bx + 3 + (int)(rnd() % (unsigned)(w - 6));
            int wy = SCR_H - h + 6 + (int)(rnd() % (unsigned)(h - 8));
            tint(wx, wy, 2, 3, pal[rnd() % 3], 180);
        }
        if ((rnd() % 4) == 0) {
            int ax = bx + w / 2;
            fill(ax, SCR_H - h - 8, 1, 8, RGB(11, 11, 23));
            fill(ax - 1, SCR_H - h - 10, 3, 2, C_MAGENTA);
        }
        bx += w + 1 + (int)(rnd() % 3);
    }

    panel(LP_X, PN_Y, PN_W, PN_H);
    panel(RP_X, PN_Y, PN_W, PN_H);

    /* рамка поля + свечение */
    for (i = 1; i <= 4; i++)
        outline(BX - 2 - i, BY - 2 - i, BW + 4 + 2 * i, BH + 4 + 2 * i, C_CYAN, 44 - i * 9);
    outline(BX - 2, BY - 2, BW + 4, BH + 4, C_CYAN, 160);
    outline(BX - 1, BY - 1, BW + 2, BH + 2, C_CYAN, 160);

    /* левая панель */
    text(14, 14, "TETRIS", 3, C_YELLOW);
    text(14, 40, "// HACK", 1, C_MAGENTA);
    text(26, 54, "NET", 1, C_CYAN);
    text(14, 72, "HOLD", 1, C_MUTED);
    slot(LCX, 100);
    text(14, 126, "SCORE", 1, C_MUTED);
    text(14, 170, "LINES", 1, C_MUTED);
    text(14, 206, "LEVEL", 1, C_MUTED);

    /* правая панель */
    text(RP_X + 6, 14, "NEXT", 1, C_MUTED);
    slot(RCX, 40);
    slot(RCX, 76);
    slot(RCX, 112);
    text(RP_X + 6, 142, "BEST", 1, C_MUTED);
    text(RP_X + 6, 192, "UP: HARD DROP", 1, C_MUTED);
    text(RP_X + 6, 204, "X/O: ROTATE", 1, C_MUTED);
    text(RP_X + 6, 216, "SQ: ROTATE BACK", 1, C_MUTED);
    text(RP_X + 6, 228, "L/R/TRI: HOLD", 1, C_MUTED);
    text(RP_X + 6, 240, "START: PAUSE", 1, C_MUTED);
    text(RP_X + 6, 252, "SELECT: RESTART", 1, C_MUTED);

    rng = saved;
    T = frame;
}

static void build_boardbg(void)
{
    int x, y;
    for (y = 0; y < BH; y++) {
        for (x = 0; x < BW; x++) {
            u32 c = RGB(5, 5, 11);
            float dx = (x - BW / 2.0f) / (BW / 2.0f), dy = (y - BH / 2.0f) / (BH / 2.0f);
            float r = sqrtf(dx * dx + dy * dy);
            float t = (r - 0.55f) / 0.9f;
            if ((x % CELL) == 0 && x > 0) c = mix(c, C_CYAN, 19);
            if ((y % CELL) == 0 && y > 0) c = mix(c, C_CYAN, 19);
            if (t > 0) { if (t > 1) t = 1; c = mix(c, C_BLACK, (int)(t * 0.5f * 256)); }
            boardbg[y * BW + x] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Игровая логика                                                      */
/* ------------------------------------------------------------------ */
enum { IN_LEFT = 1, IN_RIGHT = 2, IN_DOWN = 4, IN_UP = 8, IN_ROT_CW = 16,
       IN_ROT_CCW = 32, IN_HOLD = 64, IN_START = 128, IN_RESTART = 256 };

typedef struct { float x, y, vx, vy, life, max; u32 col; int size; } Part;
enum { S_READY, S_PLAYING, S_PAUSED, S_CLEAR, S_OVER };

#define MAXPART 200

static unsigned char grid[ROWS][COLS];       /* 0 - пусто, иначе тип+1 */
static int pt, pr, px, py;                   /* текущая фигура */
static int hold, hold_used;
static int nextq[4];
static int bag[7], bag_i = 7;
static int score, best, lines, level, combo, state;
static double tnow, overAt, grav_acc, lock_t, clear_t, das_t, arr_t;
static int das_dir, resets, lowest;
static unsigned char clear_rows[ROWS];
static int nclear;
static Part parts[MAXPART];
static int nparts;
static char savepath[320] = "ms0:/PSP/GAME/Tetris/tetris.sav";

static void load_save(void)
{
    FILE *f = fopen(savepath, "r");
    int b = 0;
    if (f) { if (fscanf(f, "%d", &b) != 1) b = 0; fclose(f); }
    best = b < 0 ? 0 : b;
}

static void write_save(void)
{
    FILE *f = fopen(savepath, "w");
    if (f) { fprintf(f, "%d\n", best); fclose(f); }
}

/* --- столкновения --- */
static int collide(int t, int r, int x, int y)
{
    int i;
    for (i = 0; i < 4; i++) {
        int cx = x + shape[t][r][i][0], cy = y + shape[t][r][i][1];
        if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) return 1;
        if (grid[cy][cx]) return 1;
    }
    return 0;
}

static int on_ground(void) { return collide(pt, pr, px, py + 1); }

static int ghost_y(void)
{
    int y = py;
    while (!collide(pt, pr, px, y + 1)) y++;
    return y;
}

/* --- мешок из 7 фигур --- */
static int next_piece(void)
{
    if (bag_i >= 7) {
        int i;
        for (i = 0; i < 7; i++) bag[i] = i;
        for (i = 6; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        bag_i = 0;
    }
    return bag[bag_i++];
}

static float rnd_range(float a, float b) { return a + rndf() * (b - a); }

static void burst_row(int r)
{
    static const u32 cols[3] = { C_CYAN, C_YELLOW, C_MAGENTA };
    int i;
    for (i = 0; i < 14 && nparts < MAXPART; i++) {
        Part *p = &parts[nparts++];
        float life = 450.0f + rndf() * 350.0f;
        p->x = rndf() * BW;
        p->y = (float)((r - HID) * CELL + CELL / 2);
        p->vx = rnd_range(-60.0f, 60.0f);
        p->vy = rnd_range(-90.0f, 40.0f);
        p->life = p->max = life;
        p->col = cols[i % 3];
        p->size = 2 + (int)(rndf() * 2.0f);
    }
}

static double gravity_ms(void)
{
    int l = level > 20 ? 20 : level;
    double ms = pow(0.8 - (l - 1) * 0.007, l - 1) * 1000.0;
    return ms < 17.0 ? 17.0 : ms;
}

static void end_game(void)
{
    state = S_OVER;
    overAt = tnow;
    if (score > best) { best = score; write_save(); }
}

static void spawn_piece(int t)
{
    pt = t; pr = 0; px = 3; py = 1;
    grav_acc = 0; lock_t = 0; resets = 0; lowest = py;
    hold_used = 0;
    if (collide(pt, pr, px, py)) { end_game(); return; }
    if (!on_ground()) { py++; lowest = py; }          /* стандартный первый шаг вниз */
}

static void spawn_next(void)
{
    int t = nextq[0], i;
    for (i = 0; i < 3; i++) nextq[i] = nextq[i + 1];
    nextq[3] = next_piece();
    spawn_piece(t);
}

static void new_game(void)
{
    int i;
    memset(grid, 0, sizeof grid);
    memset(clear_rows, 0, sizeof clear_rows);
    nclear = 0;
    score = 0; lines = 0; level = 1; combo = -1;
    hold = -1; hold_used = 0;
    pt = -1;
    bag_i = 7;
    for (i = 0; i < 4; i++) nextq[i] = next_piece();
    nparts = 0;
    das_dir = 0; das_t = 0; arr_t = 0;
    grav_acc = 0; lock_t = 0; resets = 0;
    state = S_READY;
}

static void start_game(void)
{
    state = S_PLAYING;
    spawn_next();
}

/* --- фиксация фигуры и линии --- */
static void collapse(void)
{
    int r, w = ROWS - 1;
    for (r = ROWS - 1; r >= 0; r--) {
        if (!clear_rows[r]) {
            if (w != r) memcpy(grid[w], grid[r], COLS);
            w--;
        }
    }
    for (; w >= 0; w--) memset(grid[w], 0, COLS);
    memset(clear_rows, 0, sizeof clear_rows);
    nclear = 0;
}

static void lock_piece(void)
{
    static const int tbl[5] = { 0, 100, 300, 500, 800 };
    int i, r, c;
    for (i = 0; i < 4; i++) {
        int cx = px + shape[pt][pr][i][0], cy = py + shape[pt][pr][i][1];
        grid[cy][cx] = (unsigned char)(pt + 1);
    }
    pt = -1;
    nclear = 0;
    memset(clear_rows, 0, sizeof clear_rows);
    for (r = 0; r < ROWS; r++) {
        int full = 1;
        for (c = 0; c < COLS; c++) if (!grid[r][c]) { full = 0; break; }
        if (full) { clear_rows[r] = 1; nclear++; }
    }
    if (nclear) {
        int add = tbl[nclear] * level;
        combo++;
        if (combo > 0) add += 50 * combo * level;
        score += add;
        if (score > 999999) score = 999999;
        lines += nclear;
        level = 1 + lines / 10;
        if (level > 30) level = 30;
        for (r = 0; r < ROWS; r++) if (clear_rows[r]) burst_row(r);
        state = S_CLEAR;
        clear_t = 0;
    } else {
        combo = -1;
        spawn_next();
    }
}

/* --- действия игрока --- */
static void lock_reset_if_grounded(void)
{
    if (on_ground() && resets < 15) { lock_t = 0; resets++; }
}

static void try_move(int dx)
{
    if (!collide(pt, pr, px + dx, py)) { px += dx; lock_reset_if_grounded(); }
}

static void try_rotate(int cw)
{
    int nr = cw ? (pr + 1) & 3 : (pr + 3) & 3;
    int idx = cw ? pr * 2 : ((pr + 3) & 3) * 2 + 1;
    int k;
    if (pt == P_O) return;
    for (k = 0; k < 5; k++) {
        const int8_t *kk = (pt == P_I) ? kick_i[idx][k] : kick_jlstz[idx][k];
        int nx = px + kk[0], ny = py - kk[1];      /* SRS: y вверх */
        if (!collide(pt, nr, nx, ny)) {
            px = nx; py = ny; pr = nr;
            if (py > lowest) { lowest = py; resets = 0; }
            lock_reset_if_grounded();
            return;
        }
    }
}

static void do_hold(void)
{
    int t;
    if (hold_used) return;
    t = hold;
    hold = pt;
    if (t < 0) spawn_next(); else spawn_piece(t);
    hold_used = 1;
}

static void hard_drop(void)
{
    while (!collide(pt, pr, px, py + 1)) { py++; score += 2; }
    if (score > 999999) score = 999999;
    lock_piece();
}

static void handle_move(unsigned held, unsigned pressed, double dt)
{
    if (pressed & IN_LEFT)  { das_dir = -1; das_t = 0; arr_t = 0; try_move(-1); }
    else if (pressed & IN_RIGHT) { das_dir = 1; das_t = 0; arr_t = 0; try_move(1); }

    if (das_dir == -1 && !(held & IN_LEFT))  { das_dir = (held & IN_RIGHT) ? 1 : 0; das_t = 0; arr_t = 0; }
    if (das_dir == 1  && !(held & IN_RIGHT)) { das_dir = (held & IN_LEFT) ? -1 : 0; das_t = 0; arr_t = 0; }

    if (das_dir) {
        das_t += dt;
        if (das_t >= DAS_MS) {
            arr_t += dt;
            while (arr_t >= ARR_MS) { arr_t -= ARR_MS; try_move(das_dir); }
        }
    }
}

static void update_piece(double dt, int soft)
{
    double iv = gravity_ms();
    if (soft) { double s = iv / 20.0; if (s < 20.0) s = 20.0; if (s < iv) iv = s; }
    grav_acc += dt;
    while (grav_acc >= iv) {
        grav_acc -= iv;
        if (!on_ground()) {
            py++;
            if (soft) { score++; if (score > 999999) score = 999999; }
            if (py > lowest) { lowest = py; resets = 0; }
            lock_t = 0;
        } else { grav_acc = 0; break; }
    }
    if (on_ground()) {
        lock_t += dt;
        if (lock_t >= LOCK_MS) lock_piece();
    } else lock_t = 0;
}

/* --- публичный интерфейс для платформенного слоя --- */
static void game_init(u32 seed)
{
    if (seed) rng = seed;
    init_shapes();
    build_bg();
    build_boardbg();
    load_save();
    tnow = 0; overAt = 0;
    new_game();
}

static void game_update(double dt, unsigned held, unsigned pressed)
{
    int i;
    tnow += dt;

    for (i = nparts - 1; i >= 0; i--) {
        Part *p = &parts[i];
        p->life -= (float)dt;
        if (p->life <= 0) { parts[i] = parts[--nparts]; continue; }
        p->x += p->vx * (float)dt / 1000.0f;
        p->y += p->vy * (float)dt / 1000.0f;
        p->vy += 120.0f * (float)dt / 1000.0f;        /* лёгкая гравитация искр */
    }

    if (pressed & IN_RESTART) { new_game(); return; }

    switch (state) {
    case S_READY:
        if (pressed & (IN_START | IN_ROT_CW)) start_game();
        break;
    case S_PAUSED:
        if (pressed & IN_START) state = S_PLAYING;
        break;
    case S_OVER:
        if (tnow - overAt >= 700 && (pressed & (IN_START | IN_ROT_CW))) { new_game(); start_game(); }
        break;
    case S_CLEAR:
        clear_t += dt;
        if (clear_t >= CLEAR_MS) {
            collapse();
            state = S_PLAYING;
            spawn_next();
        }
        break;
    case S_PLAYING:
        if (pressed & IN_START) { state = S_PAUSED; break; }
        if (pressed & IN_HOLD) do_hold();
        if (state != S_PLAYING) break;
        if (pressed & IN_ROT_CW)  try_rotate(1);
        if (pressed & IN_ROT_CCW) try_rotate(0);
        handle_move(held, pressed, dt);
        if (pressed & IN_UP) { hard_drop(); break; }
        update_piece(dt, (held & IN_DOWN) != 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Отрисовка кадра                                                     */
/* ------------------------------------------------------------------ */
static void cell_xy(int c, int r, int *x, int *y)
{
    *x = BX + c * CELL;
    *y = BY + (r - HID) * CELL;
}

static void draw_grid(void)
{
    int r, c, x, y;
    for (r = HID; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            if (grid[r][c]) { cell_xy(c, r, &x, &y); block(x, y, CELL, PCOL[grid[r][c] - 1]); }
}

static void draw_piece_cells(int t, int r, int ox, int oy, int ghost)
{
    int i, x, y;
    for (i = 0; i < 4; i++) {
        int cx = ox + shape[t][r][i][0], cy = oy + shape[t][r][i][1];
        if (cy < HID) continue;
        cell_xy(cx, cy, &x, &y);
        if (ghost) {
            tint(x + 1, y + 1, CELL - 2, CELL - 2, PCOL[t], 26);
            outline(x + 1, y + 1, CELL - 2, CELL - 2, PCOL[t], 150);
        } else block(x, y, CELL, PCOL[t]);
    }
}

static void draw_clear_flash(void)
{
    int r, phase = ((int)(clear_t / 55.0)) & 1;
    u32 col = phase ? C_WHITE : C_CYAN;
    int a = 150 + (int)(60.0 * (1.0 - clear_t / CLEAR_MS));
    for (r = HID; r < ROWS; r++)
        if (clear_rows[r]) {
            int x, y;
            cell_xy(0, r, &x, &y);
            tint(x, y, BW, CELL, col, a > 256 ? 256 : a);
        }
}

static void draw_particles(void)
{
    int i;
    for (i = 0; i < nparts; i++) {
        Part *p = &parts[i];
        int a = (int)(p->life / p->max * 256.0f);
        if (a < 0) a = 0;
        brect((int)(p->x - p->size / 2.0f), (int)(p->y - p->size / 2.0f), p->size, p->size, p->col, a);
    }
}

static void glitch(void)
{
    static u32 tmp[BW];
    int i, r, x;
    double k = tnow - overAt;
    if (state != S_OVER || k > 700) return;
    for (i = 0; i < 5; i++) {
        int y = (int)(rnd() % BH);
        int h = 3 + (int)(rnd() % 20);
        int dx = (int)(rnd() % 21) - 10;
        for (r = y; r < y + h && r < BH; r++) {
            u32 *row = frame + (BY + r) * BUF_W + BX;
            memcpy(tmp, row, sizeof tmp);
            for (x = 0; x < BW; x++) {
                int sx = x - dx;
                if (sx >= 0 && sx < BW) row[x] = tmp[sx];
            }
        }
    }
    brect(0, (int)(rnd() % BH), BW, 3 + (int)(rnd() % 14), C_MAGENTA, 36);
    brect(0, (int)(rnd() % BH), BW, 2 + (int)(rnd() % 10), C_CYAN, 31);
}

static void draw_overlay(void)
{
    char buf[48];
    int cx = BX + BW / 2, cy = BY + BH / 2;
    int show = state == S_READY || state == S_PAUSED ||
               (state == S_OVER && tnow - overAt >= 700);
    if (!show) return;
    tint(BX, BY, BW, BH, RGB(5, 5, 11), state == S_PAUSED ? 240 : 215);

    if (state == S_READY) {
        title_c(cx, cy - 60, "TETRIS", 3, C_YELLOW);
        text_c(cx, cy - 20, "STACK THE", 1, C_TEXT);
        text_c(cx, cy - 8, "ENGRAMS", 2, C_CYAN);
        text_c(cx, cy + 40, "PRESS X", 2, C_CYAN);
        text_c(cx, cy + 58, "OR START", 2, C_CYAN);
    } else if (state == S_PAUSED) {
        title_c(cx, cy - 40, "PAUSE", 3, C_YELLOW);
        text_c(cx, cy, "CONNECTION HELD", 1, C_TEXT);
        text_c(cx, cy + 40, "START: RESUME", 1, C_CYAN);
    } else {
        title_c(cx, cy - 70, "FLATLINE", 2, C_YELLOW);
        text_c(cx, cy - 40, "CONNECTION LOST", 1, C_MAGENTA);
        snprintf(buf, sizeof buf, "SCORE %06d", score);
        text_c(cx, cy - 16, buf, 1, C_TEXT);
        snprintf(buf, sizeof buf, "LINES %d", lines);
        text_c(cx, cy - 2, buf, 1, C_TEXT);
        if (score > 0 && score >= best) text_c(cx, cy + 20, "NEW BEST!", 2, C_CYAN);
        text_c(cx, cy + 60, "X/START: RETRY", 1, C_CYAN);
    }
}

static void draw_hud(void)
{
    char buf[24];
    int i;
    float op = (cosf((float)(tnow / 1600.0 * 6.2831853)) + 1.0f) / 2.0f * 0.75f + 0.25f;
    snprintf(buf, sizeof buf, "%06d", score);
    text(14, 138, buf, 3, C_YELLOW);
    snprintf(buf, sizeof buf, "%d", lines);
    text(14, 182, buf, 2, C_TEXT);
    snprintf(buf, sizeof buf, "%d", level);
    text(14, 218, buf, 2, C_CYAN);
    snprintf(buf, sizeof buf, "%06d", best);
    text(RP_X + 6, 154, buf, 3, C_CYAN);
    tint(14, 53, 7, 7, C_CYAN, (int)(op * 256));           /* индикатор сети */

    draw_preview(hold, LCX, 100, 10, hold_used);
    for (i = 0; i < 3; i++) draw_preview(nextq[i], RCX, 40 + i * 36, 10, 0);
}

static void render(void)
{
    int r;
    T = frame;
    memcpy(frame, bg, sizeof frame);
    for (r = 0; r < BH; r++)
        memcpy(frame + (BY + r) * BUF_W + BX, boardbg + r * BW, BW * sizeof(u32));

    draw_grid();
    if (pt >= 0 && (state == S_PLAYING || state == S_PAUSED || state == S_OVER)) {
        int gy = ghost_y();
        if (gy != py && state == S_PLAYING) draw_piece_cells(pt, pr, px, gy, 1);
        draw_piece_cells(pt, pr, px, py, 0);
    }
    if (state == S_CLEAR) draw_clear_flash();
    draw_particles();
    for (r = 0; r < BH; r += 3) tint(BX, BY + r, BW, 1, C_BLACK, 56);   /* сканлайны */
    glitch();
    draw_hud();
    draw_overlay();
}

/* ------------------------------------------------------------------ */
/* Платформенный слой PSP                                              */
/* ------------------------------------------------------------------ */
#ifndef HOST_TEST

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cbid;
    (void)args; (void)argp;
    cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}

/* сохранение лежит рядом с EBOOT.PBP */
static void set_save_dir(const char *argv0)
{
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash && (size_t)(slash - argv0) + 12 < sizeof savepath) {
        size_t n = (size_t)(slash - argv0) + 1;
        memcpy(savepath, argv0, n);
        strcpy(savepath + n, "tetris.sav");
    }
}

int main(int argc, char **argv)
{
    SceCtrlData pad;
    unsigned int old = 0;
    u32 *vram_cached = (u32 *)sceGeEdramGetAddr();
    u32 *vram_uncached = (u32 *)((unsigned int)vram_cached | 0x40000000);
    unsigned long long last;

    setup_callbacks();
    if (argc > 0) set_save_dir(argv[0]);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    sceDisplaySetMode(0, SCR_W, SCR_H);
    sceDisplaySetFrameBuf(vram_cached, BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);

    last = (unsigned long long)sceKernelGetSystemTimeWide();
    game_init((u32)last | 1u);
    last = (unsigned long long)sceKernelGetSystemTimeWide();

    for (;;) {
        unsigned long long now;
        unsigned int held = 0, pressed;
        int ax, ay;
        double dt;

        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_LEFT)    held |= IN_LEFT;
        if (pad.Buttons & PSP_CTRL_RIGHT)   held |= IN_RIGHT;
        if (pad.Buttons & PSP_CTRL_DOWN)    held |= IN_DOWN;
        if (pad.Buttons & PSP_CTRL_UP)      held |= IN_UP;
        if (pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) held |= IN_ROT_CW;
        if (pad.Buttons & PSP_CTRL_SQUARE)  held |= IN_ROT_CCW;
        if (pad.Buttons & (PSP_CTRL_TRIANGLE | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) held |= IN_HOLD;
        if (pad.Buttons & PSP_CTRL_START)   held |= IN_START;
        if (pad.Buttons & PSP_CTRL_SELECT)  held |= IN_RESTART;

        ax = (int)pad.Lx - 128; ay = (int)pad.Ly - 128;
        if (ax < -60) held |= IN_LEFT;
        if (ax >  60) held |= IN_RIGHT;
        if (ay >  60) held |= IN_DOWN;

        pressed = held & ~old;
        old = held;

        now = (unsigned long long)sceKernelGetSystemTimeWide();
        dt = (double)(now - last) / 1000.0;
        last = now;
        if (dt > 100.0) dt = 100.0;
        game_update(dt, held, pressed);

        render();
        sceDisplayWaitVblankStart();
        memcpy(vram_uncached, frame, sizeof frame);
    }
    return 0;
}

#endif /* !HOST_TEST */
