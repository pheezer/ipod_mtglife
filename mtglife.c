/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * MTG Life - Magic: The Gathering life & counter tracker for the click wheel.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "plugin.h"
#include "lib/configfile.h"

#define MAX_PLAYERS       6
#define MIN_PLAYERS       2
#define DEFAULT_PLAYERS   4
#define DEFAULT_LIFE      40
#define POISON_LETHAL     10
#define CMD_LETHAL        21
#define NUM_FIXED         4
#define MAX_COUNTERS      (NUM_FIXED + MAX_PLAYERS - 1)
#define CFG_FILE          "mtglife.cfg"

#define COL_HILITE   LCD_RGBPACK(255, 210, 0)    /* gold: counter picker row */
#define COL_ACTIVE   LCD_RGBPACK(0, 200, 255)    /* cyan: active counter row */
#define COL_WARN     LCD_RGBPACK(255, 60, 60)    /* red: lethal / dead */
#define COL_DIM      LCD_RGBPACK(170, 170, 170)
#define COL_UP       LCD_RGBPACK(40, 220, 60)    /* green: life increase */
#define COL_DOWN     LCD_RGBPACK(255, 70, 70)    /* red: life decrease */
#define COL_RING     LCD_RGBPACK(255, 255, 255)  /* white: selection ring */
#define COL_DEAD     LCD_RGBPACK(100, 0, 0)      /* very dark red: life below 0 */

#define ANIM_PULSE   (HZ / 3)      /* flash duration after a change */
#define ANIM_HOLD    (5 * HZ)      /* life tally shown fully this long */
#define ANIM_FADE    (HZ)          /* then fades to nothing over this long */
#define ANIM_FRAME   (HZ / 25)     /* frame tick while something animates */
#define NUM_SCALE    2             /* life number drawn at this integer scale */

#define RFF_ROLL     (3 * HZ / 2)  /* "rolling for first" blink phase */
#define RFF_WIN      (2 * HZ)      /* winner blinks fast */
#define RFF_FADE     (3 * HZ / 2)  /* winner holds "1st" then fades out */

/* per-player signature colors (whole-cell background = identity).
 * Kept as RGB triples so we can lighten/darken for the flash. */
struct rgb { short r, g, b; };
static const struct rgb PLAYER_RGB[MAX_PLAYERS] = {
    {210, 50, 50},   {60, 110, 230},  {50, 175, 80},
    {220, 175, 30},  {165, 70, 210},  {235, 130, 30},
};  /* P1 red, P2 blue, P3 green, P4 amber, P5 purple, P6 orange */

/* Loaded from the bundled Adobe-Helvetica-Bold set at startup; fall back to
 * FONT_UI if the .fnt isn't installed on the device. */
static int font_big = FONT_UI;   /* life number while actively changing / dice */
static int font_num = FONT_UI;   /* life number at rest (a bit smaller) */
static int font_med = FONT_UI;   /* labels, tally, counter screen */

enum mode { MODE_GRID, MODE_LIFE, MODE_PICKER, MODE_COUNTER };

struct player {
    int life;
    int poison, energy, experience, commander_tax;
    int cmd_dmg[MAX_PLAYERS];   /* damage taken FROM player k's commander */
    int  delta;                 /* running life tally within the active window */
    long delta_tick;            /* tick of last life change */
    long pulse_tick;            /* tick of last change (drives the flash) */
    int  pulse_dir;             /* +1 / -1 direction of last change */
};

static struct game {
    int num_players, start_life;
    struct player p[MAX_PLAYERS];
    int sel;                    /* highlighted / active player */
    enum mode mode;
    /* counter picker state, rebuilt per player */
    char clabel[MAX_COUNTERS][16];
    int *cptr[MAX_COUNTERS];
    int cleth[MAX_COUNTERS];
    int counter_count;
    int picker_sel;
    int active_counter;
    long cnt_pulse_tick;        /* flash on the active counter row */
    int  cnt_pulse_dir;
    bool rff_active;            /* "roll for first" animation playing */
    long rff_start;             /* tick it began */
    int  rff_winner;            /* chosen first player */
} g;

static int die_idx = 6;         /* index into die_sides -> d20 */

static struct configdata config[] = {
    {TYPE_INT, MIN_PLAYERS, MAX_PLAYERS, {.int_p = &g.num_players}, "players", NULL},
    {TYPE_INT, 1, 99, {.int_p = &g.start_life}, "start_life", NULL},
    {TYPE_INT, 0, 7, {.int_p = &die_idx}, "die", NULL},
};
#define CFG_COUNT (int)(sizeof(config)/sizeof(config[0]))

static void init_player(int i)
{
    int j;
    g.p[i].life = g.start_life;
    g.p[i].poison = g.p[i].energy = g.p[i].experience = g.p[i].commander_tax = 0;
    for (j = 0; j < MAX_PLAYERS; j++)
        g.p[i].cmd_dmg[j] = 0;
    g.p[i].delta = g.p[i].pulse_dir = 0;
    g.p[i].delta_tick = g.p[i].pulse_tick = 0;
}

static void reset_life(void)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++)
        init_player(i);
}

static void build_counter_list(int who)
{
    int k;
    g.counter_count = 0;

#define ADDC(lbl, ptr, leth) do { \
        rb->snprintf(g.clabel[g.counter_count], sizeof(g.clabel[0]), "%s", (lbl)); \
        g.cptr[g.counter_count] = (ptr); \
        g.cleth[g.counter_count] = (leth); \
        g.counter_count++; \
    } while (0)

    ADDC("Poison",     &g.p[who].poison,        POISON_LETHAL);
    ADDC("Energy",     &g.p[who].energy,        0);
    ADDC("Experience", &g.p[who].experience,    0);
    ADDC("Cmd tax",    &g.p[who].commander_tax, 0);

    for (k = 0; k < g.num_players; k++) {
        if (k == who)
            continue;
        rb->snprintf(g.clabel[g.counter_count], sizeof(g.clabel[0]),
                     "Cmd from P%d", k + 1);
        g.cptr[g.counter_count]  = &g.p[who].cmd_dmg[k];
        g.cleth[g.counter_count] = CMD_LETHAL;
        g.counter_count++;
    }
#undef ADDC
}

/* Fill the whole screen with no blank cells: columns-per-row by player count.
 * Returns the number of rows and fills rowcols[] with the columns in each. */
static int clampi(int v)        { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int lum(struct rgb c)    { return (77 * c.r + 150 * c.g + 29 * c.b) >> 8; }

static int grid_layout(int n, int rowcols[2])
{
    switch (n) {
        case 2:  rowcols[0] = 1; rowcols[1] = 1;   return 2;  /* top / bottom halves */
        case 3:  rowcols[0] = 2; rowcols[1] = 1;   return 2;  /* two quarters + half  */
        case 4:  rowcols[0] = 2; rowcols[1] = 2;   return 2;
        case 5:  rowcols[0] = 2; rowcols[1] = 3;   return 2;
        case 6:  rowcols[0] = 3; rowcols[1] = 3;   return 2;
        default: rowcols[0] = (n < 1) ? 1 : n;     return 1;
    }
}

/* Clockwise order of the player cells: top row left->right, then the
 * bottom row right->left, which traces the perimeter like going around a
 * table. Fills ring[] (ring[position] = player index). */
static void build_ring(int ring[])
{
    int rowcols[2], rows = grid_layout(g.num_players, rowcols);
    int r, c, base = 0, pos = 0;
    for (r = 0; r < rows; r++) {
        int cols = rowcols[r];
        if ((r & 1) == 0)
            for (c = 0; c < cols; c++)       ring[pos++] = base + c;
        else
            for (c = cols - 1; c >= 0; c--)  ring[pos++] = base + c;
        base += cols;
    }
}

/* Draw a string enlarged by `scale`, optionally flipped 180 degrees (so a
 * player across the table reads their total right-way-up). Rockbox has no
 * scaled or rotated text, so we read each glyph's bitmap and plot every set
 * pixel as a scale x scale block. (ox,oy) is the top-left of the output box;
 * the output is (strwidth*scale) x (fontheight*scale). */
static void draw_number(int ox, int oy, const char *s, int fontid,
                        unsigned color, bool flip, int scale)
{
    struct font *pf = rb->font_get(fontid);
    int fh = pf->height, tw, th, penx, gx, gy;
    const char *p;

    rb->font_getstringsize((const unsigned char *)s, &tw, &th, fontid);
    rb->lcd_set_foreground(color);
    for (p = s, penx = 0; *p; p++) {
        unsigned short ch = (unsigned char)*p;
        int gw = rb->font_get_width(pf, ch);
        const unsigned char *bits = rb->font_get_bits(pf, ch);
        for (gy = 0; gy < fh; gy++)
            for (gx = 0; gx < gw; gx++)
                if ((bits[(gy >> 3) * gw + gx] >> (gy & 7)) & 1) {
                    int sx = penx + gx;                 /* pixel in the text box */
                    int px = flip ? (tw - 1 - sx) : sx;
                    int py = flip ? (fh - 1 - gy) : gy;
                    rb->lcd_fillrect(ox + px * scale, oy + py * scale, scale, scale);
                }
        penx += gw;
    }
}

/* Draw s horizontally centered within [x, x+w) at top y, flipped if asked. */
static void draw_centered(int x, int w, int y, const char *s, int fontid,
                          unsigned color, bool flip)
{
    int sw, sh;
    rb->font_getstringsize((const unsigned char *)s, &sw, &sh, fontid);
    if (flip)
        draw_number(x + (w - sw) / 2, y, s, fontid, color, true, 1);
    else {
        rb->lcd_setfont(fontid);
        rb->lcd_set_foreground(color);
        rb->lcd_putsxy(x + (w - sw) / 2, y, s);
    }
}

/* Like draw_centered, but with a 1px outline so it reads on any background. */
static void draw_centered_outlined(int x, int w, int y, const char *s, int fontid,
                                   unsigned fg, unsigned outline, bool flip)
{
    int sw, sh, dx, dy, nx;
    rb->font_getstringsize((const unsigned char *)s, &sw, &sh, fontid);
    nx = x + (w - sw) / 2;
    rb->lcd_setfont(fontid);
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx || dy) {
                if (flip) draw_number(nx + dx, y + dy, s, fontid, outline, true, 1);
                else { rb->lcd_set_foreground(outline); rb->lcd_putsxy(nx + dx, y + dy, s); }
            }
    if (flip) draw_number(nx, y, s, fontid, fg, true, 1);
    else { rb->lcd_set_foreground(fg); rb->lcd_putsxy(nx, y, s); }
}

static void draw_cell(int idx, int x, int y, int w, int h, long now, bool flip)
{
    struct player *pl = &g.p[idx];
    struct rgb c = PLAYER_RGB[idx];
    long page = now - pl->pulse_tick;
    int br = c.r, bg = c.g, bb = c.b;
    unsigned ink;
    int fw, fh, k, maxcmd = 0, bp = 0, num_y = 0, num_h = 0;
    char buf[8], bd[40];
    long age;

    /* whole-cell background IS the player color; on a change it flashes
     * briefly lighter (life up) or darker (life down). */
    if (page < ANIM_PULSE) {
        int kf = 60 * (ANIM_PULSE - page) / ANIM_PULSE;   /* 0..60% */
        if (pl->pulse_dir > 0) { br += (255-br)*kf/100; bg += (255-bg)*kf/100; bb += (255-bb)*kf/100; }
        else                   { br -= br*kf/100;       bg -= bg*kf/100;       bb -= bb*kf/100; }
    }
    {
        unsigned cellcol = LCD_RGBPACK(clampi(br), clampi(bg), clampi(bb));
        rb->lcd_set_foreground(cellcol);
        rb->lcd_fillrect(x, y, w, h);
        /* text is drawn in DRMODE_SOLID, which paints glyph "paper" with the
         * background color -- make it the cell color so ink (incl. black on a
         * light panel) shows instead of black-on-black. */
        rb->lcd_set_background(cellcol);
    }
    rb->lcd_set_foreground(LCD_BLACK);                     /* 1px seam between cells */
    rb->lcd_drawrect(x, y, w, h);

    ink = (lum(c) < 140) ? LCD_WHITE : LCD_BLACK;          /* readable on this color */

    /* life number: large, centered, facing the player (top row flipped) */
    {
        unsigned col = (pl->life < 0) ? COL_DEAD : ink;
        int ow;
        rb->snprintf(buf, sizeof buf, "%d", pl->life);
        rb->font_getstringsize((const unsigned char *)buf, &fw, &fh, font_num);
        ow    = fw * NUM_SCALE;
        num_h = fh * NUM_SCALE;
        num_y = y + (h - num_h) / 2;
        draw_number(x + (w - ow) / 2, num_y, buf, font_num, col, flip, NUM_SCALE);
    }

    /* player label: pulled toward the screen centre (where the cells
     * converge), facing the player (flipped for the top row) */
    {
        int lh = rb->font_get(font_med)->height, lw, lsh, lx, ly, cx = x + w / 2;
        char lbl[8];
        rb->snprintf(lbl, sizeof lbl, "P%d", idx + 1);
        rb->font_getstringsize((const unsigned char *)lbl, &lw, &lsh, font_med);
        if (cx < LCD_WIDTH / 2 - 4)      lx = x + w - lw - 4;   /* cell left of centre: hug right */
        else if (cx > LCD_WIDTH / 2 + 4) lx = x + 4;            /* cell right of centre: hug left */
        else                             lx = x + (w - lw) / 2; /* straddles centre */
        ly = flip ? (y + h - lh - 2) : (y + 2);                /* inner (centre) edge */
        if (flip)
            draw_number(lx, ly, lbl, font_med, ink, true, 1);
        else {
            rb->lcd_setfont(font_med);
            rb->lcd_set_foreground(ink);
            rb->lcd_putsxy(lx, ly, lbl);
        }
    }

    /* running life-change tally: centered just "above" the number (above for
     * upright players, below on-screen for a flipped top-row one); fades out. */
    age = now - pl->delta_tick;
    if (pl->delta != 0 && age < ANIM_HOLD + ANIM_FADE) {
        char ts[8];
        int lh = rb->font_get(font_med)->height, pop, ty, f = 100;
        int tcr, tcg, tcb, ocr, ocg, ocb;
        rb->snprintf(ts, sizeof ts, "%+d", pl->delta);
        if (age >= ANIM_HOLD) {                            /* fading phase */
            long rem = ANIM_HOLD + ANIM_FADE - age;
            f = 100 * rem / ANIM_FADE;                     /* 100 -> 0 */
        }
        /* green (up) / red (down), blended toward the cell color as it fades */
        if (pl->delta > 0) { tcr = 60;  tcg = 230; tcb = 90; }
        else               { tcr = 255; tcg = 80;  tcb = 80; }
        tcr = c.r + (tcr - c.r) * f / 100;
        tcg = c.g + (tcg - c.g) * f / 100;
        tcb = c.b + (tcb - c.b) * f / 100;
        ocr = c.r * (100 - f) / 100;                       /* outline: black -> bg */
        ocg = c.g * (100 - f) / 100;
        ocb = c.b * (100 - f) / 100;
        pop = (age < HZ / 8) ? 2 : 0;                      /* brief pop on change */
        ty  = flip ? (num_y + num_h + 2 + pop) : (num_y - lh - 2 - pop);
        draw_centered_outlined(x, w, ty, ts, font_med,
                               LCD_RGBPACK(clampi(tcr), clampi(tcg), clampi(tcb)),
                               LCD_RGBPACK(clampi(ocr), clampi(ocg), clampi(ocb)), flip);
    } else if (pl->delta != 0) {
        pl->delta = 0;                                     /* window elapsed */
    }

    /* counter badges along the bottom (nonzero only); '!' marks lethal */
    for (k = 0; k < g.num_players; k++)
        if (pl->cmd_dmg[k] > maxcmd)
            maxcmd = pl->cmd_dmg[k];
    bd[0] = '\0';
    if (pl->poison)        bp += rb->snprintf(bd+bp, sizeof bd-bp, "Po%d%s ", pl->poison, pl->poison>=POISON_LETHAL?"!":"");
    if (maxcmd)            bp += rb->snprintf(bd+bp, sizeof bd-bp, "Cd%d%s ", maxcmd, maxcmd>=CMD_LETHAL?"!":"");
    if (pl->energy)        bp += rb->snprintf(bd+bp, sizeof bd-bp, "En%d ", pl->energy);
    if (pl->experience)    bp += rb->snprintf(bd+bp, sizeof bd-bp, "Ex%d ", pl->experience);
    if (pl->commander_tax) bp += rb->snprintf(bd+bp, sizeof bd-bp, "Tx%d ", pl->commander_tax);
    if (bd[0]) {
        /* counters sit at the player's near edge -- top of screen (flipped) for
         * a top-row player, bottom (upright) otherwise -- centered, facing them */
        int bh = rb->font_get(FONT_UI)->height;
        draw_centered(x, w, flip ? (y + 3) : (y + h - bh - 3), bd, FONT_UI, ink, flip);
    }

    /* selection ring on top: double black+white so it shows on any color */
    if (idx == g.sel && (g.mode == MODE_GRID || g.mode == MODE_LIFE)) {
        bool white = (g.mode == MODE_GRID) || (((now / (HZ / 4)) % 2) == 0);
        rb->lcd_set_foreground(LCD_BLACK);
        rb->lcd_drawrect(x + 2, y + 2, w - 4, h - 4);
        rb->lcd_set_foreground(white ? LCD_WHITE : LCD_BLACK);
        rb->lcd_drawrect(x + 3, y + 3, w - 6, h - 6);
        rb->lcd_drawrect(x + 4, y + 4, w - 8, h - 8);
    }
}

static void draw_grid(long now)
{
    int rowcols[2];
    int rows = grid_layout(g.num_players, rowcols);
    int rowh = LCD_HEIGHT / rows;
    int r, c, idx = 0;

    for (r = 0; r < rows; r++) {
        int cols = rowcols[r];
        int cw = LCD_WIDTH / cols;
        int y = r * rowh;
        int h = (r == rows - 1) ? (LCD_HEIGHT - y) : rowh;
        for (c = 0; c < cols && idx < g.num_players; c++, idx++) {
            int x = c * cw;
            int w = (c == cols - 1) ? (LCD_WIDTH - x) : cw;
            bool flip = (rows > 1 && r == 0);          /* top row faces across */
            draw_cell(idx, x, y, w, h, now, flip);
        }
    }
}

/* Rectangle + facing of a single player cell (same layout as draw_grid). */
static void cell_rect(int idx, int *ox, int *oy, int *ow, int *oh, bool *oflip)
{
    int rowcols[2], rows = grid_layout(g.num_players, rowcols);
    int rowh = LCD_HEIGHT / rows, r, c, i = 0;
    for (r = 0; r < rows; r++) {
        int cols = rowcols[r], cw = LCD_WIDTH / cols;
        int yy = r * rowh, hh = (r == rows - 1) ? (LCD_HEIGHT - yy) : rowh;
        for (c = 0; c < cols; c++, i++)
            if (i == idx) {
                int xx = c * cw;
                *ox = xx; *oy = yy;
                *ow = (c == cols - 1) ? (LCD_WIDTH - xx) : cw;
                *oh = hh;
                *oflip = (rows > 1 && r == 0);
                return;
            }
    }
}

/* "Roll for first" animation over the normal grid: a blinking banner, then the
 * winner's square blinks fast showing "1st", then it fades back to play mode. */
static void draw_rff_overlay(long now)
{
    long el = now - g.rff_start;

    if (el >= RFF_ROLL + RFF_WIN + RFF_FADE) {
        g.rff_active = false;                          /* finished */
        return;
    }

    if (el < RFF_ROLL) {                               /* blinking banner */
        int tw, th, by;
        rb->lcd_setfont(font_med);
        rb->lcd_getstringsize("Rolling for first", &tw, &th);
        by = LCD_HEIGHT / 2 - th / 2 - 3;
        rb->lcd_set_foreground(LCD_BLACK);
        rb->lcd_fillrect(0, by, LCD_WIDTH, th + 6);
        if (((el / (HZ / 3)) % 2) == 0) {
            rb->lcd_set_background(LCD_BLACK);
            rb->lcd_set_foreground(LCD_WHITE);
            rb->lcd_putsxy((LCD_WIDTH - tw) / 2, by + 3, "Rolling for first");
        }
        return;
    }

    {                                                  /* winner reveal */
        long wel = el - RFF_ROLL;
        bool blinking = (wel < RFF_WIN);
        bool show = blinking ? (((wel / (HZ / 6)) % 2) == 0) : true;
        if (show) {
            int wx = 0, wy = 0, ww = 0, wh = 0, sw, sh, ow, oh, f = 100;
            int gr = 255, gg = 210, gb = 0;            /* gold */
            bool wflip = false;
            struct rgb pc = PLAYER_RGB[g.rff_winner];
            unsigned fill, ink1;
            cell_rect(g.rff_winner, &wx, &wy, &ww, &wh, &wflip);
            if (!blinking) {                           /* fade gold -> player colour */
                long fel = wel - RFF_WIN;
                f = 100 * (RFF_FADE - fel) / RFF_FADE;
                gr = pc.r + (gr - pc.r) * f / 100;
                gg = pc.g + (gg - pc.g) * f / 100;
                gb = pc.b + (gb - pc.b) * f / 100;
            }
            fill = LCD_RGBPACK(clampi(gr), clampi(gg), clampi(gb));
            rb->lcd_set_foreground(fill);
            rb->lcd_fillrect(wx, wy, ww, wh);
            rb->lcd_set_background(fill);
            ink1 = blinking ? LCD_BLACK
                 : LCD_RGBPACK(clampi(gr*(100-f)/100), clampi(gg*(100-f)/100),
                               clampi(gb*(100-f)/100));
            rb->font_getstringsize((const unsigned char *)"1st", &sw, &sh, font_num);
            ow = sw * NUM_SCALE;  oh = sh * NUM_SCALE;
            draw_number(wx + (ww - ow) / 2, wy + (wh - oh) / 2, "1st", font_num,
                        ink1, wflip, NUM_SCALE);
        }
    }
}

static void draw_counter_screen(long now)
{
    int i, y, lh, tw, th;
    char line[48];

    rb->lcd_setfont(font_med);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->snprintf(line, sizeof line, "Player %d   Life %d", g.sel + 1, g.p[g.sel].life);
    rb->lcd_getstringsize(line, &tw, &th);
    rb->lcd_putsxy(4, 2, line);

    y = 2 + th + 2;
    rb->lcd_set_foreground(COL_DIM);
    rb->lcd_fillrect(0, y, LCD_WIDTH, 1);
    y += 3;

    rb->lcd_setfont(font_med);
    rb->lcd_getstringsize("Mg", &tw, &lh);
    for (i = 0; i < g.counter_count; i++) {
        int val = *g.cptr[i];
        bool cur = (g.mode == MODE_PICKER) ? (i == g.picker_sel)
                                           : (i == g.active_counter);
        bool leth = g.cleth[i] > 0 && val >= g.cleth[i];
        rb->snprintf(line, sizeof line, "%s: %d%s", g.clabel[i], val,
                     leth ? "  !" : "");
        if (cur) {
            unsigned fillc = (g.mode == MODE_COUNTER) ? COL_ACTIVE : COL_HILITE;
            if (g.mode == MODE_COUNTER && now - g.cnt_pulse_tick < ANIM_PULSE)
                fillc = g.cnt_pulse_dir > 0 ? COL_UP : COL_DOWN;   /* flash on change */
            rb->lcd_set_foreground(fillc);
            rb->lcd_fillrect(0, y - 1, LCD_WIDTH, lh + 2);
            rb->lcd_set_background(fillc);          /* paper = row fill so black text shows */
            rb->lcd_set_foreground(LCD_BLACK);
        } else {
            rb->lcd_set_background(LCD_BLACK);
            rb->lcd_set_foreground(leth ? COL_WARN : LCD_WHITE);
        }
        rb->lcd_putsxy(4, y, line);
        y += lh + 2;
    }

    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_getstringsize("Mg", &tw, &th);
    rb->lcd_set_foreground(COL_DIM);
    if (g.mode == MODE_PICKER)
        rb->lcd_putsxy(4, LCD_HEIGHT - th - 2, "SELECT:edit  MENU:back");
    else
        rb->lcd_putsxy(4, LCD_HEIGHT - th - 2, "scroll:+/-  SELECT:list  MENU:back");
}

static void draw_game(long now)
{
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    if (g.mode == MODE_PICKER || g.mode == MODE_COUNTER)
        draw_counter_screen(now);
    else {
        draw_grid(now);
        if (g.rff_active)
            draw_rff_overlay(now);
    }
    rb->lcd_update();
}

static void roll_dice(void)
{
    static const struct opt_items dt[8] = {
        {"d2",  TALK_ID(2, UNIT_INT)},  {"d4",  TALK_ID(4, UNIT_INT)},
        {"d6",  TALK_ID(6, UNIT_INT)},  {"d8",  TALK_ID(8, UNIT_INT)},
        {"d10", TALK_ID(10, UNIT_INT)}, {"d12", TALK_ID(12, UNIT_INT)},
        {"d20", TALK_ID(20, UNIT_INT)}, {"d100",TALK_ID(100, UNIT_INT)}
    };
    static const int sides[8] = {2, 4, 6, 8, 10, 12, 20, 100};

    rb->set_option("Die type", &die_idx, RB_INT, dt, 8, NULL);

    for (;;) {
        int roll = rb->rand() % sides[die_idx] + 1;
        int tw, th, b;
        char buf[16];

        rb->lcd_set_background(LCD_BLACK);
        rb->lcd_clear_display();

        rb->lcd_setfont(font_med);
        rb->snprintf(buf, sizeof buf, "d%d", sides[die_idx]);
        rb->lcd_getstringsize(buf, &tw, &th);
        rb->lcd_set_foreground(COL_DIM);
        rb->lcd_putsxy((LCD_WIDTH - tw) / 2, LCD_HEIGHT / 2 - th - 20, buf);

        rb->lcd_setfont(font_big);
        rb->snprintf(buf, sizeof buf, "%d", roll);
        rb->lcd_getstringsize(buf, &tw, &th);
        rb->lcd_set_foreground(LCD_WHITE);
        rb->lcd_putsxy((LCD_WIDTH - tw) / 2, LCD_HEIGHT / 2 - th / 2 + 4, buf);

        rb->lcd_setfont(FONT_UI);
        rb->lcd_getstringsize("Mg", &tw, &th);
        rb->lcd_set_foreground(COL_DIM);
        rb->lcd_putsxy(4, LCD_HEIGHT - th - 2, "SELECT/scroll:reroll  MENU:back");
        rb->lcd_update();

        b = rb->button_get(true);
        if (b == BUTTON_SELECT || b == BUTTON_SCROLL_FWD || b == BUTTON_SCROLL_BACK)
            continue;
        if (b == BUTTON_MENU || b == (BUTTON_MENU | BUTTON_REL))
            break;
        if (rb->default_event_handler(b) == SYS_USB_CONNECTED)
            break;
    }
    rb->button_clear_queue();
}

/* returns true if the user chose Quit */
static bool main_menu(void)
{
    int sel = 0, k, oldn;
    bool done = false, quit = false;

    MENUITEM_STRINGLIST(menu, "MTG Life", NULL,
                        "Resume", "Number of players", "Starting life",
                        "Roll dice", "Reset life", "Quit");

    rb->button_clear_queue();
    while (!done) {
        switch (rb->do_menu(&menu, &sel, NULL, false)) {
            case 0:
                done = true;
                break;
            case 1:
                oldn = g.num_players;
                rb->set_int("Number of players", "", UNIT_INT, &g.num_players,
                            NULL, 1, MIN_PLAYERS, MAX_PLAYERS, NULL);
                for (k = oldn; k < g.num_players; k++)
                    init_player(k);
                if (g.sel >= g.num_players)
                    g.sel = g.num_players - 1;
                break;
            case 2:
                rb->set_int("Starting life", "", UNIT_INT, &g.start_life,
                            NULL, 5, 1, 99, NULL);
                break;
            case 3:
                roll_dice();
                break;
            case 4:
                reset_life();
                break;
            case 5:
                done = true;
                quit = true;
                break;
            default:        /* GO_TO_PREVIOUS: cancelled with a MENU tap */
                done = true;
                break;
        }
    }
    return quit;
}

static void handle_scroll(int dir)
{
    switch (g.mode) {
        case MODE_GRID: {
            int ring[MAX_PLAYERS], n = g.num_players, i, pos = 0;
            build_ring(ring);                      /* clockwise around the screen */
            for (i = 0; i < n; i++)
                if (ring[i] == g.sel) { pos = i; break; }
            g.sel = ring[(pos + dir + n) % n];
            break;
        }
        case MODE_LIFE: {
            long now = *rb->current_tick;
            struct player *pl = &g.p[g.sel];
            pl->life += dir;
            if (now - pl->delta_tick >= ANIM_HOLD + ANIM_FADE)
                pl->delta = 0;                  /* previous window elapsed: start fresh */
            pl->delta += dir;
            pl->delta_tick = now;
            pl->pulse_tick = now;
            pl->pulse_dir = (dir > 0) ? 1 : -1;
            break;
        }
        case MODE_PICKER:
            g.picker_sel += dir;
            if (g.picker_sel < 0)
                g.picker_sel = g.counter_count - 1;
            else if (g.picker_sel >= g.counter_count)
                g.picker_sel = 0;
            break;
        case MODE_COUNTER: {
            int *v = g.cptr[g.active_counter];
            *v += dir;
            if (*v < 0)
                *v = 0;
            g.cnt_pulse_tick = *rb->current_tick;
            g.cnt_pulse_dir = (dir > 0) ? 1 : -1;
            break;
        }
    }
}

static void handle_select(void)
{
    switch (g.mode) {
        case MODE_GRID:
            g.mode = MODE_LIFE;
            break;
        case MODE_LIFE:
            build_counter_list(g.sel);
            g.picker_sel = 0;
            g.mode = MODE_PICKER;
            break;
        case MODE_PICKER:
            g.active_counter = g.picker_sel;
            g.mode = MODE_COUNTER;
            break;
        case MODE_COUNTER:
            g.mode = MODE_PICKER;
            break;
    }
}

static void handle_back(void)
{
    switch (g.mode) {
        case MODE_COUNTER: g.mode = MODE_LIFE; break;
        case MODE_PICKER:  g.mode = MODE_LIFE; break;
        case MODE_LIFE:    g.mode = MODE_GRID; break;
        case MODE_GRID:    break;
    }
}

/* true while any flash/tally/pulse is in flight, so the loop runs at frame rate;
 * otherwise the loop blocks on input (no CPU spin when idle). */
static bool animating(long now)
{
    int i;
    if (g.rff_active)
        return true;                                        /* roll-for-first running */
    if (g.mode == MODE_LIFE)
        return true;                                        /* pulsing edit ring */
    if (g.mode == MODE_COUNTER && now - g.cnt_pulse_tick < ANIM_PULSE)
        return true;
    for (i = 0; i < g.num_players; i++) {
        if (now - g.p[i].pulse_tick < ANIM_PULSE)
            return true;
        if (g.p[i].delta != 0 && now - g.p[i].delta_tick < ANIM_HOLD + ANIM_FADE)
            return true;
    }
    return false;
}

enum plugin_status plugin_start(const void *parameter)
{
    int btn;
    bool quit = false, saw_repeat = false;

    (void)parameter;

    g.num_players = DEFAULT_PLAYERS;
    g.start_life  = DEFAULT_LIFE;
    rb->srand(*rb->current_tick);

    configfile_load(CFG_FILE, config, CFG_COUNT, 0);
    if (g.num_players < MIN_PLAYERS) g.num_players = MIN_PLAYERS;
    if (g.num_players > MAX_PLAYERS) g.num_players = MAX_PLAYERS;

    /* large bold fonts for readability; fall back to the UI font if absent */
    font_big = rb->font_load("/.rockbox/fonts/35-Adobe-Helvetica-Bold.fnt");
    font_num = rb->font_load("/.rockbox/fonts/27-Adobe-Helvetica-Bold.fnt");
    font_med = rb->font_load("/.rockbox/fonts/18-Adobe-Helvetica-Bold.fnt");
    if (font_big < 0) font_big = FONT_UI;
    if (font_num < 0) font_num = font_big;   /* no size change if 27px missing */
    if (font_med < 0) font_med = FONT_UI;

    reset_life();
    g.sel = 0;
    g.mode = MODE_GRID;

    rb->splash(HZ, "MTG Life  -  hold MENU for setup");

    while (!quit) {
        long now = *rb->current_tick;
        rb->reset_poweroff_timer();                 /* don't idle-poweroff while open */
        rb->backlight_on();                         /* keep the screen lit */
        draw_game(now);
        /* never block forever: wake at least once a second to keep awake */
        btn = rb->button_get_w_tmo(animating(now) ? ANIM_FRAME : HZ);
        if (btn == BUTTON_NONE)
            continue;                               /* timeout: keep-alive + animate */

        if (btn == (BUTTON_MENU | BUTTON_REPEAT)) {
            saw_repeat = true;                      /* long press detected */
        } else if (btn == (BUTTON_MENU | BUTTON_REL)) {
            if (saw_repeat) {                       /* released a hold -> main menu */
                if (main_menu())
                    quit = true;
                rb->button_clear_queue();
            } else {
                handle_back();                      /* quick tap -> go back */
            }
            saw_repeat = false;
        } else if (btn == BUTTON_SELECT) {
            handle_select();
        } else if (btn == BUTTON_SCROLL_FWD || btn == (BUTTON_SCROLL_FWD | BUTTON_REPEAT)) {
            handle_scroll(+1);
        } else if (btn == BUTTON_SCROLL_BACK || btn == (BUTTON_SCROLL_BACK | BUTTON_REPEAT)) {
            handle_scroll(-1);
        } else if (btn == BUTTON_PLAY) {
            g.rff_winner = rb->rand() % g.num_players;   /* PLAY = roll for first */
            g.rff_start  = now;
            g.rff_active = true;
            g.mode = MODE_GRID;
        } else if (btn == BUTTON_MENU ||
                   btn == (BUTTON_SELECT | BUTTON_REL) ||
                   btn == (BUTTON_SELECT | BUTTON_REPEAT)) {
            /* press/hold edges handled on their companion event; ignore */
        } else if (rb->default_event_handler(btn) == SYS_USB_CONNECTED) {
            quit = true;
        }
    }

    configfile_save(CFG_FILE, config, CFG_COUNT, 0);
    if (font_big != FONT_UI) rb->font_unload(font_big);
    if (font_num != FONT_UI && font_num != font_big) rb->font_unload(font_num);
    if (font_med != FONT_UI) rb->font_unload(font_med);
    return PLUGIN_OK;
}
