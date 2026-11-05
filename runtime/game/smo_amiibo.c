/* smo_amiibo.c - the amiibo menu.
 *
 * On the console amiibo are read by the controller's NFC reader: the player holds right on the +Control Pad (the game's
 * amiibo mode) or answers the amiibo NPC, and taps a figure to the reader. Here a key (F1 by default) or a button
 * combination of a pad (both stick buttons by default; controllers.ini [hotkeys]) opens a menu drawn like the game's own
 * pause menu, from the game's own layout textures, fonts and messages (smo_ui.c), listing every amiibo the game has a name
 * for, in its language. The game is held while the menu is open: nvn_gl.c queue_present calls amiibo_menu_run after a
 * presented frame, and display.c stops signalling the game's vsync event (it would count the refreshes and run ahead to
 * catch up). Up / down (stick, +Control Pad, keyboard) select, L / R or ZL / ZR move by a page, A puts the amiibo on the
 * reader (nnapplet.c nfp_place_amiibo), B or the key again closes the menu.
 *
 * Then the game reads the amiibo the next time it looks for one. If it was looking for one already, i.e. its amiibo mode
 * (HelpAmiiboDirector) or the amiibo NPC's touch prompt (EventFlowNodeAmiiboTouchLayout) was waiting for → in the frames
 * after the menu closed, right is held for the player (nnhid.c) until the game has read the amiibo, as a player would hold
 * it while tapping the figure; otherwise the amiibo stays on the reader until the player holds right in the game.
 *
 * The list: the labels of AmiiboName.msbt name every amiibo the game knows, id_0xGGCCVV (game, character, variant) or
 * id_0xGGCCVV_NNN (and the model number, for the three Wedding amiibo), in the order of the file; to them the Gold Mario
 * amiibo is added (model number 60, which the Gold Mario Suit asks for: ItemCloth.byml), named after Mario and that
 * suit. The tag's identification block: GG CC VV 00 (figure), the model number big-endian, series 0, 02.
 *
 * Look (measured on the game's own pause menu, 1600x900 captures; layout units of 1280 x 720, y up): the list turned 3
 * degrees about (-590, -19), rows 54 apart; the chosen row in a white bar 484 x 56 with rounded corners and a soft shadow,
 * the cap cursor (CmCursor00) 37 units into it, its name in TitleFont60, dark grey; the other rows white with a soft
 * shadow; the logo top left (the game's "x amiibo" logo of its amiibo guide, CmTitleLogoxAmiibo, where that guide puts it);
 * the footer: white, 58 high, a gold line (186, 160, 0) on top, the guide text right-aligned at x 580 in MessageFont38
 * with the button pictures of DeviceFont80, gold. On the right, where the pause menu shows Mario and Cappy: the figure
 * the game shows in its amiibo guide for Mario and the three Wedding amiibo, the amiibo symbol for the others. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "smo_ui.h"

void nfp_place_amiibo(const uint8_t *id);                 /* nnapplet.c */
long nfp_amiibo_reads(void);
int nerve_watch_set(const char *a, const char *b);       /* smo_nerve.c */
long long nerve_watch_last(void);
long long nvn_front_frame_next(void);                    /* nvn_gl.c */
int nvn_menu_job(UiMenuJob *m);
uint64_t hid_menu_buttons(void);                         /* nnhid.c */
void hid_menu_closed(void);
void hid_amiibo_hold_after(long long frame, long reads);
int hid_menu_key(int which);
void display_menu_hold(int on);                          /* display.c */
void display_menu_wait(void);
int display_closing(void);

enum { B_A = 0, B_B = 1, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9, B_UP = 13, B_DOWN = 15, B_SL_UP = 17, B_SL_DOWN = 19 };   /* nnhid.c button bits */
#define BIT(b) (1ull << (b))

typedef struct { uint16_t name[64]; uint8_t id[8]; int title_font, picture; } Amiibo;
enum { PIC_ICON, PIC_MARIO, PIC_MARIO_W, PIC_PEACH_W, PIC_KOOPA_W, PIC_N };
static const char *const g_pic_tex[PIC_N] = {"MyIconAmiibo^w", "MyImageAmiibo^w", "MyImageAmiiboMario^w", "MyImageAmiiboPeach^w", "MyImageAmiiboKoopa^w"};

static struct {
    int tried, ok;
    Amiibo *list; int n;
    UiFont *title, *msg, *dev;
    UiImage cursor, corner, corner_sh, logo, arrow, circle, pic[PIC_N];
    uint16_t footer[256];
    float sel_scale, row_scale, msg_scale, msg_per_title; /* layout units per font pixel */
    UiImage bg, frame;
    int sel, top;
} M;
static volatile int32_t g_request;

#define VIS 7                                             /* rows shown */
#define ROW_STEP 54.0f
#define ROW0 135.0f                                       /* local y of the first row shown */
#define LIST_X (-590.0f)
#define LIST_Y (-19.0f)
#define ITEM_X 4.0f

/* ---------------------------------------------------------------- the game's files (smo_ui.c) */
#define lang_folder ui_lang_folder
#define lang_arc ui_lang_arc
#define layout_tex ui_layout_tex
#define font_of ui_font_in
#define has_glyphs ui_has_glyphs
static void u16cpy(uint16_t *d, const uint16_t *s, int cap) { int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static void u16cat(uint16_t *d, const uint16_t *s, int cap) { int i = 0; while (d[i]) i++; u16cpy(d + i, s, cap - i); }
static void u16cat_a(uint16_t *d, const char *s, int cap) { int i = 0; while (d[i]) i++; for (; *s && i < cap - 1; s++) d[i++] = (uint8_t)*s; d[i] = 0; }

static void add_amiibo(int gg, int cc, int vv, int num, const uint16_t *name, const uint16_t *extra) {
    Amiibo *a = realloc(M.list, sizeof *M.list * (size_t)(M.n + 1));
    if (!a) return;
    M.list = a;
    a = &M.list[M.n++];
    memset(a, 0, sizeof *a);
    u16cpy(a->name, name, 64);
    if (extra) { u16cat_a(a->name, " (", 64); u16cat(a->name, extra, 64); u16cat_a(a->name, ")", 64); }
    int same = 1;                                          /* the game names some variants alike (two Yoshi, two Rosalina): numbered */
    for (int i = 0; i < M.n - 1; i++) {
        int k = 0;
        while (M.list[i].name[k] && M.list[i].name[k] == name[k]) k++;
        same += !extra && M.list[i].name[k] == 0 && name[k] == 0 ? 1 : 0;
        if (!extra && name[k] == 0 && M.list[i].name[k] == ' ' && M.list[i].name[k + 1] == '(' && M.list[i].name[k + 2] >= '2' && M.list[i].name[k + 2] <= '9') same++;
    }
    if (same > 1) { char num[8]; snprintf(num, sizeof num, " (%d)", same); u16cat_a(a->name, num, 64); }
    a->id[0] = (uint8_t)gg; a->id[1] = (uint8_t)cc; a->id[2] = (uint8_t)vv; a->id[3] = 0;
    a->id[4] = (uint8_t)(num >> 8); a->id[5] = (uint8_t)num; a->id[6] = 0; a->id[7] = 2;
    int mario = gg == 0 && cc == 0 && vv == 0;
    a->picture = mario && num == 881 ? PIC_MARIO_W : gg == 0 && cc == 2 && num == 882 ? PIC_PEACH_W : gg == 0 && cc == 5 && num == 883 ? PIC_KOOPA_W : mario ? PIC_MARIO : PIC_ICON;
}

static int menu_load(void) {
    if (M.tried) return M.ok;
    M.tried = 1;
    size_t sn = 0, fn = 0;
    uint8_t *sys = lang_arc("MessageData/SystemMessage.szs", &sn), *fonts = lang_arc("LayoutData/FontData.szs", &fn);
    size_t an = 0, foot_n = 0, cn = 0;
    const uint8_t *names = sys ? ui_sarc(sys, sn, "AmiiboName.msbt", &an) : NULL, *foot = sys ? ui_sarc(sys, sn, "Footer.msbt", &foot_n) : NULL,
                  *cloth = sys ? ui_sarc(sys, sn, "ItemCloth.msbt", &cn) : NULL;
    UiMsbt *mn = names ? ui_msbt(names, an) : NULL, *mf = foot ? ui_msbt(foot, foot_n) : NULL, *mc = cloth ? ui_msbt(cloth, cn) : NULL;
    if (fonts) { M.title = font_of(fonts, fn, "TitleFont60.bffnt"); M.msg = font_of(fonts, fn, "MessageFont38.bffnt"); M.dev = font_of(fonts, fn, "DeviceFont80.bffnt"); }
    const uint16_t *mario = NULL;
    for (int i = 0; i < ui_msbt_count(mn); i++) {
        const char *l = ui_msbt_label(mn, i);
        unsigned id = 0; int num = 0;
        if (sscanf(l, "id_0x%6x_%d", &id, &num) < 1 || strncmp(l, "id_0x", 5) || strlen(l) < 11) continue;
        add_amiibo((int)(id >> 16), (int)(id >> 8) & 255, (int)id & 255, num, ui_msbt_text_at(mn, i), NULL);
        if (id == 0 && num == 0) mario = ui_msbt_text_at(mn, i);
    }
    const uint16_t *gold = ui_msbt_text(mc, "MarioColorGold");
    if (mario && gold) add_amiibo(0, 0, 0, 60, mario, gold);
    u16cpy(M.footer, ui_msbt_text(mf, "Choice_Back_Decide"), 256);
    M.ok = M.n > 0 && M.title && M.msg && M.dev &&
           layout_tex("CursorParts", "CmCursor00^w", &M.cursor) && layout_tex("SelectMenuParts", "CmSelectCorner^s", &M.corner) &&
           layout_tex("SelectMenuParts", "CmSelectCornerSh^s", &M.corner_sh) && layout_tex("ControllerGuideAmiibo", "CmTitleLogoxAmiibo^w", &M.logo) &&
           layout_tex("CursorArrowParts", "CmArrow^s", &M.arrow) && layout_tex("CursorArrowParts", "CmCircle24^s", &M.circle) &&
           layout_tex("AmiiboModeParts", g_pic_tex[PIC_ICON], &M.pic[PIC_ICON]);
    for (int p = 1; M.ok && p < PIC_N; p++) if (!layout_tex("ControllerGuideAmiibo", g_pic_tex[p], &M.pic[p])) M.pic[p] = M.pic[PIC_ICON];
    if (M.ok) {
        UiFontInfo t, s;
        ui_font_info(M.title, &t); ui_font_info(M.msg, &s);
        /* the pause menu's sizes, measured on its captures: capitals 29.3 units high in the chosen row, 21.3 in the others
         * (TitleFont60's are 46 pixels), the footer's 16.8 (MessageFont38's 32); a name the title font cannot draw is
         * written in MessageFont38 at the same cell height */
        M.sel_scale = 29.3f / 46.0f;
        M.row_scale = 21.3f / 46.0f;
        M.msg_scale = 16.8f / 32.0f;
        M.msg_per_title = (float)t.cell_h / (float)s.cell_h;
        for (int i = 0; i < M.n; i++) M.list[i].title_font = has_glyphs(M.title, M.list[i].name);
    }
    printf("amiibo menu: %s; %d amiibo (language folder %s)\n", M.ok ? "ready" : "NOT available (files of the game missing, see above)", M.n, lang_folder());
    ui_msbt_free(mn); ui_msbt_free(mf); ui_msbt_free(mc);
    free(sys); free(fonts);
    return M.ok;
}

/* ---------------------------------------------------------------- the picture (blur, windows, captions: smo_ui.c) */
static void menu_background(const uint8_t *src, int w, int h) { ui_backdrop(src, w, h, &M.bg, w, h); }
#define window ui_window
#define shadow_text ui_shadow_text

static void menu_draw(void) {
    UiImage *d = &M.frame;
    if (d->w != M.bg.w || d->h != M.bg.h || !d->px) { ui_image_free(d); d->w = M.bg.w; d->h = M.bg.h; d->ch = 4; d->px = malloc((size_t)d->w * d->h * 4); }
    if (!d->px || !M.bg.px) return;
    memcpy(d->px, M.bg.px, (size_t)d->w * d->h * 4);
    UiXf scr = ui_xf_screen(d->w, d->h);
    /* the logo, as the game's amiibo guide places it (left edge -606, centre 276, 0.7 of 660 x 176) */
    ui_draw(d, scr, -606.0f, 276.0f - 61.6f, -606.0f + 462.0f, 276.0f + 61.6f, &M.logo, 0, 0, (float)M.logo.w, (float)M.logo.h, 0xffffffff);
    /* the picture on the right */
    const Amiibo *cur = &M.list[M.sel];
    const UiImage *pic = &M.pic[cur->picture];
    if (cur->picture == PIC_ICON) ui_draw(d, scr, 340.0f - 150.0f, -30.0f - 150.0f, 340.0f + 150.0f, -30.0f + 150.0f, pic, 0, 0, (float)pic->w, (float)pic->h, 0xffffffff);
    else {                                                 /* the figure without the arrow under it (the bottom fifth of the picture) */
        float vh = (float)pic->h * 0.8f, k = 470.0f / vh, pw = (float)pic->w * k;
        ui_draw(d, scr, 330.0f - pw * 0.5f, -265.0f, 330.0f + pw * 0.5f, -265.0f + vh * k, pic, 0, 0, (float)pic->w, vh, 0xffffffff);
    }
    /* the list */
    UiXf lx = ui_xf_mul(scr, ui_xf_mul(ui_xf_move(LIST_X, LIST_Y), ui_xf_rotate(3.0f)));
    for (int r = 0; r < VIS && M.top + r < M.n; r++) {
        const Amiibo *a = &M.list[M.top + r];
        int chosen = M.top + r == M.sel;
        float cy = ROW0 - ROW_STEP * (float)r, sc = chosen ? M.sel_scale : M.row_scale;
        const UiFont *f = a->title_font ? M.title : M.msg;
        if (!a->title_font) sc *= M.msg_per_title;
        UiFontInfo fi; ui_font_info(f, &fi);
        /* the capitals (about 0.65 of the ascent) centred 4 units above the middle of the row, as in the pause menu; a name
         * wider than the bar's text box (400 units from x 74) is narrowed to fit it */
        float base = cy + 4.0f - (float)fi.ascent * 0.33f * sc, tx = ITEM_X + 74.0f, tw = ui_text(NULL, lx, f, NULL, a->name, -1, 0, 0, sc, 0);
        UiXf txf = lx;
        if (tw > 400.0f) txf = ui_xf_mul(lx, ui_xf_mul(ui_xf_move(tx, 0), ui_xf_mul(ui_xf_scale(400.0f / tw, 1.0f), ui_xf_move(-tx, 0))));
        if (chosen) {
            window(d, lx, ITEM_X + 2.0f, cy - 28.0f - 5.0f, ITEM_X + 484.0f + 3.0f, cy + 28.0f - 3.0f, &M.corner_sh, 12.0f, 0x00000050);
            window(d, lx, ITEM_X, cy - 28.0f, ITEM_X + 484.0f, cy + 28.0f, &M.corner, 6.0f, 0xffffffff);
            float cw = (float)M.cursor.w * 0.7f, ch = (float)M.cursor.h * 0.7f, ccx = ITEM_X + 37.0f, ccy = cy - 2.0f;
            ui_draw(d, lx, ccx - cw * 0.5f + 1.5f, ccy - ch * 0.5f - 2.0f, ccx + cw * 0.5f + 1.5f, ccy + ch * 0.5f - 2.0f, &M.cursor, 0, 0, (float)M.cursor.w, (float)M.cursor.h, 0x00000030);
            ui_draw(d, lx, ccx - cw * 0.5f, ccy - ch * 0.5f, ccx + cw * 0.5f, ccy + ch * 0.5f, &M.cursor, 0, 0, (float)M.cursor.w, (float)M.cursor.h, 0xffffffff);
            ui_text(d, txf, f, NULL, a->name, -1, tx, base, sc, 0x4d4241ff);
        } else shadow_text(d, txf, f, NULL, a->name, tx, base, sc, 0xffffffff);
    }
    /* more above / below: the game's list arrow buttons (CursorArrowParts: a white circle of four mirrored quarters, its
     * shadow, the arrow), turned to point up or down */
    for (int k = 0; k < 2; k++) {
        if (k == 0 ? M.top == 0 : M.top + VIS >= M.n) continue;
        float ax = ITEM_X + 74.0f + 150.0f, ay = k == 0 ? ROW0 + 42.0f : ROW0 - ROW_STEP * (float)(VIS - 1) - 42.0f, rr = 16.0f;
        float cw = (float)M.circle.w, ch = (float)M.circle.h;
        for (int pass = 0; pass < 2; pass++) {
            float ox = pass ? 0 : 1.0f, oy = pass ? 0 : -2.5f;
            uint32_t col = pass ? 0xffffffff : 0x00000048;
            ui_draw_edges(d, lx, ax - rr + ox, ay + oy, ax + ox, ay + rr + oy, &M.circle, 0, 0, cw, ch, col, UI_EDGE_LEFT | UI_EDGE_TOP);
            ui_draw_edges(d, lx, ax + ox, ay + oy, ax + rr + ox, ay + rr + oy, &M.circle, cw, 0, 0, ch, col, UI_EDGE_RIGHT | UI_EDGE_TOP);
            ui_draw_edges(d, lx, ax - rr + ox, ay - rr + oy, ax + ox, ay + oy, &M.circle, 0, ch, cw, 0, col, UI_EDGE_LEFT | UI_EDGE_BOTTOM);
            ui_draw_edges(d, lx, ax + ox, ay - rr + oy, ax + rr + ox, ay + oy, &M.circle, cw, ch, 0, 0, col, UI_EDGE_RIGHT | UI_EDGE_BOTTOM);
        }
        UiXf ax_xf = ui_xf_mul(lx, ui_xf_mul(ui_xf_move(ax, ay), ui_xf_rotate(k == 0 ? 90.0f : -90.0f)));
        ui_draw(d, ax_xf, -10.0f, -10.0f, 10.0f, 10.0f, &M.arrow, 0, 0, (float)M.arrow.w, (float)M.arrow.h, 0x4d4241ff);
    }
    /* the footer: the device icon of a Pro Controller on the left, the guide on the right, in the footer's gold */
    ui_draw(d, scr, -640.0f, -360.0f, 640.0f, -302.0f, NULL, 0, 0, 0, 0, 0xffffffff);
    ui_draw(d, scr, -640.0f, -306.0f, 640.0f, -302.0f, NULL, 0, 0, 0, 0, 0xbaa000ff);
    static const uint16_t pro[2] = {0xe12c, 0};
    ui_text(d, scr, M.dev, NULL, pro, 1, -592.0f, -344.0f, 0.45f, 0xbaa000ff);
    float fw = ui_text(NULL, scr, M.msg, M.dev, M.footer, -1, 0, 0, M.msg_scale, 0);
    ui_text(d, scr, M.msg, M.dev, M.footer, -1, 578.0f - fw, -339.0f, M.msg_scale, 0xbaa000ff);
}

/* ---------------------------------------------------------------- the menu */
void amiibo_menu_request(void) { plat_xchg(&g_request, 1); }
int amiibo_menu_wanted(void) { return g_request != 0; }
int amiibo_menu_take_request(void) { return plat_xchg(&g_request, 0) != 0; }      /* tests: asked for since the last call */

static void keep_visible(void) {                          /* the chosen row at least two rows from the edges of the window, but at the ends */
    if (M.sel < 0) M.sel = 0;
    if (M.sel >= M.n) M.sel = M.n - 1;
    if (M.sel - 2 < M.top) M.top = M.sel - 2;
    if (M.sel + 2 >= M.top + VIS) M.top = M.sel + 3 - VIS;
    if (M.top > M.n - VIS) M.top = M.n - VIS > 0 ? M.n - VIS : 0;
    if (M.top < 0) M.top = 0;
}

/* tests and the offline check: one frame of the menu over the picture rgba (w x h), with entry `sel` chosen, into out */
int amiibo_menu_compose(const uint8_t *rgba, int w, int h, int sel, uint8_t *out) {
    if (!menu_load()) return 0;
    menu_background(rgba, w, h);
    M.sel = sel; keep_visible();
    menu_draw();
    if (!M.frame.px) return 0;
    memcpy(out, M.frame.px, (size_t)w * h * 4);
    return 1;
}
int amiibo_menu_count(void) { return menu_load() ? M.n : 0; }
/* tests: entry i, its identification block and its name (ASCII, '?' for other characters); 0 = no such entry */
int amiibo_menu_entry(int i, uint8_t id[8], char *name, int cap) {
    if (!menu_load() || i < 0 || i >= M.n) return 0;
    memcpy(id, M.list[i].id, 8);
    int k = 0;
    for (; M.list[i].name[k] && k < cap - 1; k++) name[k] = M.list[i].name[k] < 128 ? (char)M.list[i].name[k] : '?';
    name[k] = 0;
    return 1;
}

/* TEST AID, SMO_AMIIBO_SHOT_DIR=<folder>: the first three different pictures of each opening of the menu as PNG files */
int tex_write_png(const char *path, const uint8_t *rgba, int w, int h);
static void save_shot(int opening, int k) {
    char p[400];
    snprintf(p, sizeof p, "%s/amiibo_menu_%d_%d.png", getenv("SMO_AMIIBO_SHOT_DIR"), opening, k);
    if (tex_write_png(p, M.frame.px, M.frame.w, M.frame.h) == 0) printf("amiibo menu: picture saved to %s\n", p);
}

/* nvn_gl.c queue_present, the game's main thread, after a presented frame: the menu, until it is closed. pj = that frame. */
void amiibo_menu_run(void *pj) {
    plat_xchg(&g_request, 0);
    plat_counter t0, t1, fq;
    plat_qpc(&t0);
    UiMenuJob job;
    memset(&job, 0, sizeof job);
    job.present = pj;
    job.op = UI_MENU_READ;
    if (!menu_load()) return;
    if (!nvn_menu_job(&job) || !job.out) { printf("amiibo menu: the frame could not be read back (the menu needs the Vulkan renderer)\n"); return; }
    if (!nerve_watch_set("HelpAmiiboDirectorNrvWait", "EventFlowNodeAmiiboTouchLayoutNrvWaitTouchTrigger"))
        printf("amiibo menu: the game's amiibo states are not known (function list): right will not be held for the game\n");
    menu_background(job.out, job.out_w, job.out_h);
    free(job.out);
    keep_visible();
    display_menu_hold(1);
    menu_draw();
    plat_qpc(&t1); plat_qpf(&fq);
    printf("amiibo menu: open at frame %lld (%.0f ms from the request to the first picture)\n", nvn_front_frame_next(), (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
    uint64_t prev = hid_menu_buttons();                    /* what is held when it opens counts only once let go */
    hid_menu_key(0); hid_menu_key(1);
    int held = 0, repeat = 0, changed = 1, dirty = 0, picked = -1, shots_left = getenv("SMO_AMIIBO_SHOT_DIR") ? 3 : 0;
    static int opened;
    opened++;
    for (;;) {
        if (display_closing()) break;                      /* the window is being closed: the game has to run to answer it */
        uint64_t b = hid_menu_buttons(), press = b & ~prev;
        prev = b;
        int up = (b & (BIT(B_UP) | BIT(B_SL_UP))) != 0, down = (b & (BIT(B_DOWN) | BIT(B_SL_DOWN))) != 0, dir = up ? -1 : down ? 1 : 0;
        int step = 0;
        if (dir && (press & (BIT(B_UP) | BIT(B_SL_UP) | BIT(B_DOWN) | BIT(B_SL_DOWN)))) { step = dir; held = 0; repeat = 0; }
        else if (dir && ++held >= 30 && ++repeat % 5 == 1) step = dir;       /* the pause menu's key repeat: after 30 frames, every 5 */
        if (!dir) held = repeat = 0;
        if (press & (BIT(B_L) | BIT(B_ZL))) step = -VIS;
        if (press & (BIT(B_R) | BIT(B_ZR))) step = VIS;
        if (step) {
            int old = M.sel;
            M.sel += step;
            if (abs(step) == 1 && held == 0) { if (M.sel < 0) M.sel = M.n - 1; if (M.sel >= M.n) M.sel = 0; }   /* a press wraps round, a held direction stops at the end */
            keep_visible();
            if (M.sel != old) changed = dirty = 1;
        }
        if (press & BIT(B_A) || hid_menu_key(1)) { picked = M.sel; break; }
        if (press & BIT(B_B) || hid_menu_key(0) || g_request) { plat_xchg(&g_request, 0); break; }
        if (dirty) { menu_draw(); dirty = 0; }
        if (changed && shots_left > 0) save_shot(opened, 3 - shots_left--);
        job.op = UI_MENU_SHOW; job.rgba = M.frame.px; job.w = M.frame.w; job.h = M.frame.h; job.changed = changed;
        if (!nvn_menu_job(&job)) { printf("amiibo menu: the picture could not be shown; closed\n"); break; }
        changed = 0;
        display_menu_wait();
    }
    job.op = UI_MENU_END;
    nvn_menu_job(&job);
    display_menu_hold(0);
    hid_menu_closed();
    long long frame = nvn_front_frame_next();
    if (picked >= 0) {
        const Amiibo *a = &M.list[picked];
        char name[64];
        int k = 0;
        for (; a->name[k] && k < 63; k++) name[k] = a->name[k] < 128 ? (char)a->name[k] : '?';
        name[k] = 0;
        long reads = nfp_amiibo_reads();
        nfp_place_amiibo(a->id);
        hid_amiibo_hold_after(frame, reads);
        printf("amiibo menu: closed at frame %lld, amiibo \"%s\" put on the reader\n", frame, name);
    } else printf("amiibo menu: closed at frame %lld without a choice\n", frame);
}
