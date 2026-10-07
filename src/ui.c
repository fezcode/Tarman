#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rlgl.h"
#include "sys.h"

Theme    T;
UI       ui;
Timeline TL;

/* ---- theme ------------------------------------------------------------------ */

static Color hex(uint32_t v) { return (Color){ (v >> 16) & 255, (v >> 8) & 255, v & 255, 255 }; }

void ui_theme(bool dark)
{
    if (dark) {
        T = (Theme){
            .bg = hex(0x0B0E13), .side = hex(0x0E1218), .panel = hex(0x12171F), .card = hex(0x161C26),
            .hover = hex(0x1D2430), .press = hex(0x242C3A), .border = hex(0x232B38), .line = hex(0x1A212C),
            .text = hex(0xE8EBF1), .dim = hex(0x9AA3B2), .faint = hex(0x5F6878),
            .accent = hex(0x4CC2FF), .on_accent = hex(0x04121C),
            .cpu = hex(0x4CC2FF), .mem = hex(0xB48CFF), .disk = hex(0x4ADE80), .net = hex(0xFFB347),
            .gpu = hex(0xFF6B9A), .kernel = hex(0x1F7FB8),
            .good = hex(0x3DDC97), .warn = hex(0xFFC857), .bad = hex(0xFF5D5D), .sel = hex(0x1B3348),
            .dark = true,
        };
    } else {
        T = (Theme){
            .bg = hex(0xF3F5F8), .side = hex(0xECEFF4), .panel = hex(0xFFFFFF), .card = hex(0xFFFFFF),
            .hover = hex(0xEDF1F6), .press = hex(0xE2E8F0), .border = hex(0xDCE2EA), .line = hex(0xEDF0F4),
            .text = hex(0x18202B), .dim = hex(0x566070), .faint = hex(0x98A1AE),
            .accent = hex(0x0A7BD0), .on_accent = hex(0xFFFFFF),
            .cpu = hex(0x0A84D6), .mem = hex(0x8B5CF6), .disk = hex(0x16A34A), .net = hex(0xE08600),
            .gpu = hex(0xE0457B), .kernel = hex(0x63B3EA),
            .good = hex(0x14A06B), .warn = hex(0xC98A00), .bad = hex(0xE03B3B), .sel = hex(0xDCEBFA),
            .dark = false,
        };
    }
}

Color col_alpha(Color c, float a)
{
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    c.a = (unsigned char)(c.a * a);
    return c;
}

Color col_mix(Color a, Color b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t) };
}

Color ui_heat(float t)
{
    if (t <= 0.001f) return BLANK;
    if (t > 1) t = 1;
    /* soft amber that warms toward orange-red, like Task Manager's columns */
    Color lo = T.dark ? hex(0x3A3320) : hex(0xFFF4CE);
    Color hi = T.dark ? hex(0x7A3A1E) : hex(0xFFB07A);
    Color c = col_mix(lo, hi, sqrtf(t));
    c.a = (unsigned char)(255 * (0.35f + 0.65f * fminf(1.0f, t * 4)));
    return c;
}

/* ---- fonts -------------------------------------------------------------------- */

/* Segoe UI's ascent+descent is ~1.33 em; stb_truetype sizes by that line
 * height, so the em we ask for is scaled up by this factor. */
#define FONT_K 1.33f

typedef struct { int weight, px; Font f; } FontSlot;
static FontSlot g_fonts[48];
static int      g_nfonts;
static const char* g_font_files[FW_COUNT][3] = {
    { "C:/Windows/Fonts/segoeui.ttf", NULL, NULL },
    { "C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf", NULL },
    { "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/segoeui.ttf", NULL },
    { "C:/Windows/Fonts/CascadiaMono.ttf", "C:/Windows/Fonts/consola.ttf", NULL },
    { "C:/Windows/Fonts/SegoeIcons.ttf", "C:/Windows/Fonts/segmdl2.ttf", NULL },
};
static int  g_text_cps[512];
static int  g_ntext_cps;
static int  g_icon_cps[128];
static int  g_nicon_cps;

void ui_fonts_init(void)
{
    g_ntext_cps = 0;
    for (int c = 32; c < 127; ++c) g_text_cps[g_ntext_cps++] = c;
    for (int c = 160; c < 384; ++c) g_text_cps[g_ntext_cps++] = c;
    for (int c = 0x391; c <= 0x3C9; ++c) g_text_cps[g_ntext_cps++] = c;   /* Greek: Δ, µ-ish symbols */
    static const int extra[] = { 0x2022, 0x2026, 0x2191, 0x2193, 0x2192, 0x2190, 0x25B2, 0x25BC,
                                 0x2212, 0x2013, 0x2014, 0x2264, 0x2265, 0x2248, 0x2116, 0x00B5 };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; ++i) g_text_cps[g_ntext_cps++] = extra[i];
    static const int icons[] = {
        IC_MENU, IC_PROCESSES, IC_PERF, IC_HISTORY, IC_GLOBE, IC_DISK, IC_CHIP, IC_BOLT, IC_PEOPLE,
        IC_PERSON, IC_SERVICES, IC_SETTINGS, IC_SEARCH, IC_PAUSE, IC_PLAY, IC_CANCEL, IC_ADD, IC_RUN,
        IC_SHIELD, IC_CHEV_R, IC_CHEV_D, IC_CHEV_U, IC_FOLDER, IC_COPY, IC_INFO, IC_PIN, IC_REFRESH,
        IC_MONITOR, IC_WIFI, IC_ETHERNET, IC_CLOCK, IC_WARNING, IC_DOWNLOAD, IC_SORT, IC_SUN, IC_MOON,
        IC_LEAF, IC_CHECK, IC_LIST, IC_TREE, IC_STOP, IC_DELETE, IC_LINK, IC_POWER,
        IC_MEMORY, IC_THERMO, IC_SHIELD_WIN, IC_WIN_MIN, IC_WIN_MAX, IC_WIN_RESTORE, IC_WIN_CLOSE,
    };
    g_nicon_cps = 0;
    for (size_t i = 0; i < sizeof icons / sizeof icons[0]; ++i) g_icon_cps[g_nicon_cps++] = icons[i];
}

void ui_fonts_free(void)
{
    for (int i = 0; i < g_nfonts; ++i)
        if (g_fonts[i].f.texture.id != GetFontDefault().texture.id) UnloadFont(g_fonts[i].f);
    g_nfonts = 0;
}

/* stb_truetype rasterizes without hinting, so glyph edges carry a lot of
 * faint coverage. A mild contrast curve trims that haze; together with
 * drawing every glyph exactly 1:1 (see font_size) it reads close to
 * Windows' own grayscale text. */
static unsigned char g_lut[256];

static void build_lut(void)
{
    for (int i = 0; i < 256; ++i) {
        float a = powf(i / 255.0f, 0.86f);
        a = (a - 0.5f) * 1.2f + 0.5f;
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        g_lut[i] = (unsigned char)(a * 255 + 0.5f);
    }
}

static Font load_sharp(const char* path, int px, int* cps, int n, bool sharpen)
{
    Font f = { 0 };
    int sz = 0;
    unsigned char* data = LoadFileData(path, &sz);
    if (!data) return f;
    f.baseSize = px;
    f.glyphCount = n;
    f.glyphPadding = 4;
    f.glyphs = LoadFontData(data, sz, px, cps, n, FONT_DEFAULT);
    UnloadFileData(data);
    if (!f.glyphs) return (Font){ 0 };
    if (!g_lut[255]) build_lut();
    if (sharpen)
        for (int i = 0; i < n; ++i) {
            Image* im = &f.glyphs[i].image;
            unsigned char* p = im->data;
            if (!p || im->format != PIXELFORMAT_UNCOMPRESSED_GRAYSCALE) continue;
            for (int k = 0; k < im->width * im->height; ++k) p[k] = g_lut[p[k]];
        }
    Image atlas = GenImageFontAtlas(f.glyphs, &f.recs, n, px, 4, 0);
    f.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    return f;
}

/* The logical draw size that maps the atlas exactly 1:1 onto screen pixels. */
static float font_size(Font f) { return (float)f.baseSize / ui.scale; }

static Font font_get(int weight, float size)
{
    int px = (int)floorf(size * (weight == FW_ICON ? 1.0f : FONT_K) * ui.scale + 0.5f);
    if (px < 6) px = 6;
    for (int i = 0; i < g_nfonts; ++i)
        if (g_fonts[i].weight == weight && g_fonts[i].px == px) return g_fonts[i].f;
    Font f = { 0 };
    for (int k = 0; k < 3 && g_font_files[weight][k]; ++k) {
        if (!FileExists(g_font_files[weight][k])) continue;
        if (weight == FW_ICON) f = load_sharp(g_font_files[weight][k], px, g_icon_cps, g_nicon_cps, false);
        else f = load_sharp(g_font_files[weight][k], px, g_text_cps, g_ntext_cps, true);
        if (f.texture.id) break;
    }
    if (!f.texture.id) f = GetFontDefault();
    else SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    if (g_nfonts == (int)(sizeof g_fonts / sizeof g_fonts[0])) {
        /* cache full (many scale changes): drop everything and start over */
        ui_fonts_free();
    }
    g_fonts[g_nfonts++] = (FontSlot){ weight, px, f };
    return f;
}

static float snap(float v) { return floorf(v * ui.scale + 0.5f) / ui.scale; }

float ui_text_w(const char* s, float size, int weight)
{
    Font f = font_get(weight, size);
    return MeasureTextEx(f, s, font_size(f), 0).x;
}

void ui_text(float x, float y, const char* s, float size, int weight, Color c)
{
    Font f = font_get(weight, size);
    DrawTextEx(f, s, (Vector2){ snap(x), snap(y) }, font_size(f), 0, c);
}

void ui_text_fit(Rectangle r, const char* s, float size, int weight, Color c, int align)
{
    if (!s || !s[0] || r.width <= 4) return;
    float w = ui_text_w(s, size, weight);
    char buf[512];
    const char* draw = s;
    if (w > r.width) {
        /* binary search for the longest prefix that fits with an ellipsis */
        size_t len = strlen(s);
        if (len > sizeof buf - 4) len = sizeof buf - 4;
        size_t lo = 0, hi = len;
        float ell = ui_text_w("\xE2\x80\xA6", size, weight);
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            memcpy(buf, s, mid);
            buf[mid] = 0;
            if (ui_text_w(buf, size, weight) + ell <= r.width) lo = mid; else hi = mid - 1;
        }
        while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) --lo; /* don't split UTF-8 */
        memcpy(buf, s, lo);
        memcpy(buf + lo, "\xE2\x80\xA6", 4);
        draw = buf;
        w = ui_text_w(draw, size, weight);
    }
    float x = r.x;
    if (align == AL_CENTER) x = r.x + (r.width - w) * 0.5f;
    else if (align == AL_RIGHT) x = r.x + r.width - w;
    float lh = font_size(font_get(weight, size));
    ui_text(x, r.y + (r.height - lh) * 0.5f - size * 0.04f, draw, size, weight, c);
}

void ui_icon(float cx, float cy, int cp, float size, Color c)
{
    Font f = font_get(FW_ICON, size);
    if (f.texture.id == GetFontDefault().texture.id) return;
    int gi = GetGlyphIndex(f, cp);
    if (f.glyphs[gi].value != cp) return;
    Rectangle rec = f.recs[gi];
    float k = 1.0f / ui.scale;                /* atlas px -> logical, exactly 1:1 */
    float ox = f.glyphs[gi].offsetX * k, oy = f.glyphs[gi].offsetY * k;
    Vector2 pos = { snap(cx - ox - rec.width * k * 0.5f), snap(cy - oy - rec.height * k * 0.5f) };
    DrawTextCodepoint(f, cp, pos, font_size(f), c);
}

/* ---- primitives ----------------------------------------------------------------- */

void ui_rrect(Rectangle r, float radius, Color c)
{
    if (r.width <= 0 || r.height <= 0 || c.a == 0) return;
    float m = fminf(r.width, r.height);
    if (radius <= 0.5f || m <= 1) { DrawRectangleRec(r, c); return; }
    float rd = fminf(1.0f, radius * 2 / m);
    DrawRectangleRounded(r, rd, 8, c);
}

void ui_rrect_line(Rectangle r, float radius, float thick, Color c)
{
    if (r.width <= 0 || r.height <= 0 || c.a == 0) return;
    float m = fminf(r.width, r.height);
    float rd = fminf(1.0f, radius * 2 / m);
    DrawRectangleRoundedLinesEx(r, rd, 8, thick, c);
}

void ui_vgradient(Rectangle r, Color top, Color bottom)
{
    DrawRectangleGradientV((int)r.x, (int)r.y, (int)ceilf(r.width), (int)ceilf(r.height), top, bottom);
}

static Rectangle g_sc_stack[16];
static int       g_sc_n;

static void scissor_apply(Rectangle r)
{
    float s = ui.scale;
    int x0 = (int)floorf(r.x * s), y0 = (int)floorf(r.y * s);
    int x1 = (int)ceilf((r.x + r.width) * s), y1 = (int)ceilf((r.y + r.height) * s);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    BeginScissorMode(x0, y0, x1 - x0, y1 - y0);
}

void ui_scissor_push(Rectangle r)
{
    if (g_sc_n > 0) {
        Rectangle p = g_sc_stack[g_sc_n - 1];
        float x0 = fmaxf(r.x, p.x), y0 = fmaxf(r.y, p.y);
        float x1 = fminf(r.x + r.width, p.x + p.width), y1 = fminf(r.y + r.height, p.y + p.height);
        r = (Rectangle){ x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0) };
    }
    if (g_sc_n < 16) g_sc_stack[g_sc_n++] = r;
    scissor_apply(r);
}

void ui_scissor_pop(void)
{
    if (g_sc_n > 0) --g_sc_n;
    if (g_sc_n > 0) scissor_apply(g_sc_stack[g_sc_n - 1]);
    else EndScissorMode();
}

Rectangle rect_inset(Rectangle r, float dx, float dy)
{
    return (Rectangle){ r.x + dx, r.y + dy, fmaxf(0, r.width - 2 * dx), fmaxf(0, r.height - 2 * dy) };
}
Rectangle rect_cut_left(Rectangle* r, float w)
{
    if (w > r->width) w = r->width;
    Rectangle o = { r->x, r->y, w, r->height };
    r->x += w; r->width -= w;
    return o;
}
Rectangle rect_cut_right(Rectangle* r, float w)
{
    if (w > r->width) w = r->width;
    Rectangle o = { r->x + r->width - w, r->y, w, r->height };
    r->width -= w;
    return o;
}
Rectangle rect_cut_top(Rectangle* r, float h)
{
    if (h > r->height) h = r->height;
    Rectangle o = { r->x, r->y, r->width, h };
    r->y += h; r->height -= h;
    return o;
}
Rectangle rect_cut_bottom(Rectangle* r, float h)
{
    if (h > r->height) h = r->height;
    Rectangle o = { r->x, r->y + r->height - h, r->width, h };
    r->height -= h;
    return o;
}

/* ---- frame & input ---------------------------------------------------------------- */

void ui_set_scale(float scale)
{
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    if (fabsf(scale - ui.scale) < 0.001f) return;
    ui.scale = scale;
    ui_fonts_free();
}

void ui_frame_begin(void)
{
    ui.w = GetScreenWidth() / ui.scale;
    ui.h = GetScreenHeight() / ui.scale;
    Vector2 m = GetMousePosition();
    ui.mouse = (Vector2){ m.x / ui.scale, m.y / ui.scale };
    ui.down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    ui.pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    ui.released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    ui.rpressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    ui.time = GetTime();
    ui.dbl = false;
    if (ui.pressed) {
        float dx = ui.mouse.x - ui.last_click_pos.x, dy = ui.mouse.y - ui.last_click_pos.y;
        if (ui.time - ui.last_click_time < 0.4 && dx * dx + dy * dy < 25) { ui.dbl = true; ui.last_click_time = 0; }
        else ui.last_click_time = ui.time;
        ui.last_click_pos = ui.mouse;
    }
    ui.wheel = GetMouseWheelMove();
    if (ui.no_input) {
        ui.mouse = (Vector2){ -1000, -1000 };
        ui.down = ui.pressed = ui.released = ui.rpressed = ui.dbl = false;
        ui.wheel = 0;
    }
    ui.click_used = ui.wheel_used = ui.rclick_used = false;
    ui.layer = 0;
    memcpy(ui.overlay_prev, ui.overlay, sizeof ui.overlay);
    ui.noverlay_prev = ui.noverlay;
    ui.noverlay = 0;
    ui.modal_prev = ui.modal;
    ui.modal = false;
    ui.cursor = MOUSE_CURSOR_DEFAULT;
    ui.tip[0] = 0;
    if (!ui.down) ui.active = 0;
    ui.focus = ui.focus_next;
    if (ui.pressed) ui.focus_next = 0;  /* a click anywhere drops focus unless a textbox re-claims it */
    TL.hover = TL.hover_next;
    TL.hover_next = -1;
}

void ui_overlay(Rectangle r)
{
    if (ui.noverlay < UI_MAX_OVERLAYS) ui.overlay[ui.noverlay++] = r;
}

bool ui_input_ok(void)
{
    if (ui.layer > 0) return true;
    if (ui.modal_prev) return false;
    for (int i = 0; i < ui.noverlay_prev; ++i)
        if (CheckCollisionPointRec(ui.mouse, ui.overlay_prev[i])) return false;
    return true;
}

bool ui_hover(Rectangle r)
{
    if (ui.active) return false;
    return ui_input_ok() && CheckCollisionPointRec(ui.mouse, r);
}

bool ui_clicked(Rectangle r)
{
    if (ui.click_used || !ui.pressed || !ui_hover(r)) return false;
    ui.click_used = true;
    return true;
}

bool ui_rclicked(Rectangle r)
{
    if (ui.rclick_used || !ui.rpressed || !ui_hover(r)) return false;
    ui.rclick_used = true;
    return true;
}

bool ui_dclicked(Rectangle r)
{
    return ui.dbl && ui_hover(r);
}

bool ui_wheel(Rectangle r, float* scroll, float step)
{
    if (ui.wheel_used || ui.wheel == 0 || !ui_hover(r)) return false;
    *scroll -= ui.wheel * step;
    ui.wheel_used = true;
    return true;
}

void ui_tooltip(const char* s)
{
    snprintf(ui.tip, sizeof ui.tip, "%s", s);
    ui.tip_at = ui.mouse;
}

uint32_t ui_id(const char* s, int n)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ (uint32_t)n) * 16777619u;
    return h ? h : 1;
}

void ui_frame_end(void)
{
    if (ui.tip[0]) {
        float fs = 12.5f;
        /* multi-line: split on '\n' */
        char buf[512];
        snprintf(buf, sizeof buf, "%s", ui.tip);
        const char* lines[16];
        int nl = 0;
        char* p = buf;
        lines[nl++] = p;
        for (; *p && nl < 16; ++p) if (*p == '\n') { *p = 0; lines[nl++] = p + 1; }
        float w = 0;
        for (int i = 0; i < nl; ++i) w = fmaxf(w, ui_text_w(lines[i], fs, FW_REG));
        float lh = fs * 1.45f;
        Rectangle r = { ui.tip_at.x + 14, ui.tip_at.y + 18, w + 20, nl * lh + 12 };
        if (r.x + r.width > ui.w - 4) r.x = ui.tip_at.x - r.width - 8;
        if (r.y + r.height > ui.h - 4) r.y = ui.tip_at.y - r.height - 8;
        if (r.x < 4) r.x = 4;
        if (r.y < 4) r.y = 4;
        ui_rrect((Rectangle){ r.x, r.y + 2, r.width, r.height }, 6, col_alpha(BLACK, T.dark ? 0.45f : 0.12f));
        ui_rrect(r, 6, T.dark ? (Color){ 0x22, 0x29, 0x35, 0xF8 } : (Color){ 0xFF, 0xFF, 0xFF, 0xFA });
        ui_rrect_line(r, 6, 1, T.border);
        for (int i = 0; i < nl; ++i)
            ui_text_fit((Rectangle){ r.x + 10, r.y + 6 + i * lh, r.width - 20, lh }, lines[i], fs,
                        i == 0 && nl > 1 ? FW_SEMI : FW_REG, i == 0 ? T.text : T.dim, AL_LEFT);
    }
    SetMouseCursor(ui.cursor);
}

/* ---- widgets -------------------------------------------------------------------------- */

float ui_button_w(const char* label, int icon)
{
    float w = 24;
    if (label && label[0]) w += ui_text_w(label, 13, FW_SEMI);
    if (icon) w += label && label[0] ? 22 : 8;
    return w;
}

bool ui_button(Rectangle r, const char* label, int icon, int style, bool enabled)
{
    bool hov = enabled && ui_hover(r);
    bool click = enabled && ui_clicked(r);
    Color bg = T.card, fg = T.text, bd = T.border;
    switch (style) {
    case BTN_PRIMARY: bg = T.accent; fg = T.on_accent; bd = BLANK;
        if (hov) bg = col_mix(T.accent, WHITE, 0.12f);
        break;
    case BTN_DANGER: bg = col_alpha(T.bad, 0.16f); fg = T.bad; bd = col_alpha(T.bad, 0.35f);
        if (hov) bg = col_alpha(T.bad, 0.26f);
        break;
    case BTN_GHOST: bg = BLANK; bd = BLANK;
        if (hov) bg = T.hover;
        break;
    default:
        if (hov) bg = T.hover;
        break;
    }
    if (hov && ui.down) bg = col_mix(bg, T.press, 0.5f);
    if (!enabled) { fg = T.faint; if (style == BTN_PRIMARY) { bg = T.hover; } }
    ui_rrect(r, 6, bg);
    if (bd.a) ui_rrect_line(r, 6, 1, bd);
    float cx = r.x + 12;
    bool has_label = label && label[0];
    if (icon) {
        if (has_label) { ui_icon(cx + 7, r.y + r.height / 2, icon, 14, fg); cx += 22; }
        else ui_icon(r.x + r.width / 2, r.y + r.height / 2, icon, 14, fg);
    }
    if (has_label)
        ui_text_fit((Rectangle){ cx, r.y, r.x + r.width - cx - 10, r.height }, label, 13, FW_SEMI, fg,
                    icon ? AL_LEFT : AL_CENTER);
    if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
    return click;
}

bool ui_icon_button(Rectangle r, int icon, const char* tip, bool active)
{
    bool hov = ui_hover(r);
    if (active) ui_rrect(r, 6, col_alpha(T.accent, 0.18f));
    else if (hov) ui_rrect(r, 6, T.hover);
    ui_icon(r.x + r.width / 2, r.y + r.height / 2, icon, 15, active ? T.accent : (hov ? T.text : T.dim));
    if (hov && tip) ui_tooltip(tip);
    if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
    return ui_clicked(r);
}

bool ui_segmented(Rectangle r, const char* const* items, int n, int* sel)
{
    ui_rrect(r, 7, T.dark ? T.panel : T.hover);
    ui_rrect_line(r, 7, 1, T.border);
    float w = r.width / n;
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        Rectangle c = { r.x + w * i + 2, r.y + 2, w - 4, r.height - 4 };
        bool on = *sel == i;
        bool hov = ui_hover(c);
        if (on) ui_rrect(c, 5, T.dark ? T.press : T.panel);
        else if (hov) ui_rrect(c, 5, col_alpha(T.hover, 0.7f));
        if (on && !T.dark) ui_rrect_line(c, 5, 1, T.border);
        ui_text_fit(c, items[i], 12.5f, on ? FW_SEMI : FW_REG, on ? T.text : T.dim, AL_CENTER);
        if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (ui_clicked(c) && !on) { *sel = i; changed = true; }
    }
    return changed;
}

bool ui_checkbox(Rectangle r, const char* label, bool* v)
{
    bool hov = ui_hover(r);
    Rectangle b = { r.x, r.y + (r.height - 16) / 2, 16, 16 };
    if (*v) {
        ui_rrect(b, 4, T.accent);
        ui_icon(b.x + 8, b.y + 8, IC_CHECK, 11, T.on_accent);
    } else {
        ui_rrect(b, 4, hov ? T.hover : T.panel);
        ui_rrect_line(b, 4, 1, hov ? T.dim : T.faint);
    }
    if (label) ui_text_fit((Rectangle){ r.x + 24, r.y, r.width - 24, r.height }, label, 13, FW_REG, T.text, AL_LEFT);
    if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
    if (ui_clicked(r)) { *v = !*v; return true; }
    return false;
}

bool ui_switch(Rectangle r, bool* v)
{
    Rectangle t = { r.x, r.y + (r.height - 20) / 2, 40, 20 };
    bool hov = ui_hover(t);
    if (*v) ui_rrect(t, 10, T.accent);
    else { ui_rrect(t, 10, hov ? T.hover : T.panel); ui_rrect_line(t, 10, 1, T.dim); }
    float kx = *v ? t.x + 30 : t.x + 10;
    DrawCircleV((Vector2){ kx, t.y + 10 }, *v ? 6.5f : 5.5f, *v ? T.on_accent : T.dim);
    if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
    if (ui_clicked(t)) { *v = !*v; return true; }
    return false;
}

bool ui_textbox(Rectangle r, char* buf, int n, uint32_t id, const char* placeholder, int icon)
{
    bool hov = ui_hover(r);
    bool focused = ui.focus == id;
    bool changed = false;
    Rectangle clr = { r.x + r.width - 26, r.y + (r.height - 20) / 2, 20, 20 };
    if (buf[0] && ui.pressed && ui_hover(clr) && !ui.click_used) {
        buf[0] = 0;
        changed = true;
        ui.click_used = true;
        ui.focus_next = id;
        focused = true;
    }
    if (ui.pressed && hov && !ui.click_used) { ui.focus_next = id; focused = true; ui.click_used = true; }
    else if (focused && !(ui.pressed && !hov)) ui.focus_next = id;
    if (focused) {
        int ch;
        size_t len = strlen(buf);
        while ((ch = GetCharPressed()) != 0) {
            char enc[8];
            int el = 0;
            const char* u = CodepointToUTF8(ch, &el);
            memcpy(enc, u, (size_t)el);
            if (len + (size_t)el < (size_t)n - 1 && ch >= 32) {
                memcpy(buf + len, enc, (size_t)el);
                len += (size_t)el;
                buf[len] = 0;
                changed = true;
            }
        }
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len > 0) {
            if (ctrl) {
                while (len > 0 && buf[len - 1] == ' ') --len;
                while (len > 0 && buf[len - 1] != ' ') --len;
            } else {
                --len;
                while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) --len;
            }
            buf[len] = 0;
            changed = true;
        }
        if (ctrl && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip) {
                for (; *clip && len < (size_t)n - 1; ++clip)
                    if ((unsigned char)*clip >= 32) buf[len++] = *clip;
                buf[len] = 0;
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (buf[0]) { buf[0] = 0; changed = true; }
            else ui.focus_next = 0;
        }
    }
    ui_rrect(r, 7, focused ? (T.dark ? T.card : T.panel) : (T.dark ? T.panel : T.panel));
    ui_rrect_line(r, 7, 1, focused ? T.accent : (hov ? T.dim : T.border));
    if (focused) {
        Rectangle u = { r.x + 6, r.y + r.height - 2, r.width - 12, 2 };
        DrawRectangleRec(u, T.accent);
    }
    float x = r.x + 10;
    if (icon) { ui_icon(x + 7, r.y + r.height / 2, icon, 13, T.dim); x += 22; }
    Rectangle tr = { x, r.y, r.x + r.width - x - 26, r.height };
    if (buf[0]) {
        ui_scissor_push(tr);
        float tw = ui_text_w(buf, 13, FW_REG);
        float off = tw > tr.width - 4 ? tw - (tr.width - 4) : 0;
        ui_text_fit((Rectangle){ tr.x - off, tr.y, tw + 4, tr.height }, buf, 13, FW_REG, T.text, AL_LEFT);
        ui_scissor_pop();
        if (focused && fmod(ui.time, 1.0) < 0.55)
            DrawRectangleRec((Rectangle){ tr.x + fminf(tw, tr.width - 4) + 1, r.y + 8, 1.2f, r.height - 16 }, T.text);
        bool ch = ui_hover(clr);
        if (ch) { ui_rrect(clr, 4, T.press); ui_tooltip("Clear (Esc)"); }
        ui_icon(clr.x + 10, clr.y + 10, IC_CANCEL, 9, ch ? T.text : T.dim);
    } else {
        ui_text_fit(tr, placeholder, 13, FW_REG, T.faint, AL_LEFT);
        if (focused && fmod(ui.time, 1.0) < 0.55)
            DrawRectangleRec((Rectangle){ tr.x, r.y + 8, 1.2f, r.height - 16 }, T.text);
    }
    if (hov) ui.cursor = buf[0] && ui_hover(clr) ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_IBEAM;
    return changed;
}

void ui_scrollbar(Rectangle track, float* scroll, float content, float view, uint32_t id)
{
    float maxs = content - view;
    if (maxs <= 0) { *scroll = 0; return; }
    if (*scroll < 0) *scroll = 0;
    if (*scroll > maxs) *scroll = maxs;
    float th = fmaxf(28, track.height * view / content);
    float ty = track.y + (track.height - th) * (*scroll / maxs);
    Rectangle thumb = { track.x + 2, ty, track.width - 4, th };
    bool hov = ui_input_ok() && CheckCollisionPointRec(ui.mouse, track);
    static float grab;
    if (ui.pressed && hov && !ui.click_used) {
        if (CheckCollisionPointRec(ui.mouse, thumb)) grab = ui.mouse.y - ty;
        else grab = th / 2;
        ui.active = id;
        ui.click_used = true;
    }
    if (ui.active == id) {
        float t = (ui.mouse.y - grab - track.y) / (track.height - th);
        *scroll = fmaxf(0, fminf(1, t)) * maxs;
        ty = track.y + (track.height - th) * (*scroll / maxs);
        thumb.y = ty;
    }
    Color c = ui.active == id ? T.dim : (hov ? col_alpha(T.dim, 0.8f) : col_alpha(T.faint, 0.6f));
    float w = hov || ui.active == id ? thumb.width : thumb.width * 0.5f;
    ui_rrect((Rectangle){ thumb.x + (thumb.width - w) / 2, thumb.y, w, thumb.height }, w / 2, c);
}

void ui_bar(Rectangle r, float t, Color c)
{
    ui_rrect(r, r.height / 2, col_alpha(T.faint, 0.25f));
    if (t > 0) ui_rrect((Rectangle){ r.x, r.y, fmaxf(r.height, r.width * fminf(1, t)), r.height }, r.height / 2, c);
}

void ui_badge(float x, float cy, const char* s, Color c)
{
    float w = ui_text_w(s, 11, FW_SEMI) + 12;
    Rectangle r = { x, cy - 9, w, 18 };
    ui_rrect(r, 9, col_alpha(c, 0.16f));
    ui_text_fit(r, s, 11, FW_SEMI, c, AL_CENTER);
}

/* ---- menus ------------------------------------------------------------------------------ */

void menu_open(Menu* m, Vector2 at, int owner)
{
    m->open = true;
    m->pos = at;
    m->n = 0;
    m->sub_open = -1;
    m->owner = owner;
    m->fresh = true;
}

static MenuItem* menu_push(Menu* m)
{
    if (m->n >= MENU_MAX) return NULL;
    MenuItem* it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->parent = -1;
    it->enabled = true;
    return it;
}

int menu_add(Menu* m, const char* label, int icon, int id, bool enabled, bool checked)
{
    MenuItem* it = menu_push(m);
    if (!it) return -1;
    snprintf(it->label, sizeof it->label, "%s", label);
    it->icon = icon; it->id = id; it->enabled = enabled; it->checked = checked;
    return m->n - 1;
}

int menu_add_sub(Menu* m, const char* label, int icon)
{
    int i = menu_add(m, label, icon, -1, true, false);
    if (i >= 0) m->items[i].has_sub = true;
    return i;
}

int menu_add_child(Menu* m, int parent, const char* label, int id, bool enabled, bool checked)
{
    int i = menu_add(m, label, 0, id, enabled, checked);
    if (i >= 0) m->items[i].parent = parent;
    return i;
}

void menu_sep(Menu* m)
{
    MenuItem* it = menu_push(m);
    if (it) it->sep = true;
}

static float menu_width(Menu* m, int parent)
{
    float w = 180;
    for (int i = 0; i < m->n; ++i)
        if (m->items[i].parent == parent && !m->items[i].sep)
            w = fmaxf(w, ui_text_w(m->items[i].label, 13, FW_REG) + 80);
    return w;
}

static float menu_height(Menu* m, int parent)
{
    float h = 8;
    for (int i = 0; i < m->n; ++i)
        if (m->items[i].parent == parent) h += m->items[i].sep ? 9 : 30;
    return h;
}

static int menu_panel(Menu* m, int parent, Rectangle box, Rectangle* sub_anchor)
{
    ui_overlay(box);
    ui_rrect((Rectangle){ box.x, box.y + 3, box.width, box.height }, 8, col_alpha(BLACK, T.dark ? 0.5f : 0.15f));
    ui_rrect(box, 8, T.dark ? (Color){ 0x1B, 0x21, 0x2B, 0xFF } : T.panel);
    ui_rrect_line(box, 8, 1, T.border);
    float y = box.y + 4;
    int clicked = -1;
    for (int i = 0; i < m->n; ++i) {
        MenuItem* it = &m->items[i];
        if (it->parent != parent) continue;
        if (it->sep) {
            DrawRectangleRec((Rectangle){ box.x + 10, y + 4, box.width - 20, 1 }, T.border);
            y += 9;
            continue;
        }
        Rectangle r = { box.x + 4, y, box.width - 8, 30 };
        bool hov = it->enabled && ui_hover(r);
        if (hov || m->sub_open == i) ui_rrect(r, 5, T.hover);
        Color fg = it->enabled ? T.text : T.faint;
        if (it->checked) ui_icon(r.x + 16, r.y + 15, IC_CHECK, 12, T.accent);
        else if (it->icon) ui_icon(r.x + 16, r.y + 15, it->icon, 13, it->enabled ? T.dim : T.faint);
        ui_text_fit((Rectangle){ r.x + 34, r.y, r.width - 50, r.height }, it->label, 13, FW_REG, fg, AL_LEFT);
        if (it->has_sub) {
            ui_icon(r.x + r.width - 14, r.y + 15, IC_CHEV_R, 10, T.dim);
            if (hov) { m->sub_open = i; *sub_anchor = r; }
            if (m->sub_open == i) *sub_anchor = r;
        } else if (hov && parent < 0) {
            m->sub_open = -1;
        }
        if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (!it->has_sub && it->enabled && ui_clicked(r)) clicked = it->id;
        y += 30;
    }
    return clicked;
}

int menu_run(Menu* m)
{
    if (!m->open) return -1;
    int saved = ui.layer;
    ui.layer = 1;
    float w = menu_width(m, -1), h = menu_height(m, -1);
    Rectangle box = { m->pos.x, m->pos.y, w, h };
    if (box.x + w > ui.w - 6) box.x = ui.w - 6 - w;
    if (box.y + h > ui.h - 6) box.y = ui.h - 6 - h;
    if (box.x < 6) box.x = 6;
    if (box.y < 6) box.y = 6;
    Rectangle anchor = { 0 };
    int res = menu_panel(m, -1, box, &anchor);
    if (m->sub_open >= 0 && res < 0) {
        float sw = menu_width(m, m->sub_open), sh = menu_height(m, m->sub_open);
        Rectangle sb = { anchor.x + anchor.width + 6, anchor.y - 4, sw, sh };
        if (sb.x + sw > ui.w - 6) sb.x = box.x - sw - 2;
        if (sb.y + sh > ui.h - 6) sb.y = ui.h - 6 - sh;
        int r2 = menu_panel(m, m->sub_open, sb, &anchor);
        if (r2 >= 0) res = r2;
    }
    bool inside = false;
    for (int i = 0; i < ui.noverlay; ++i) if (CheckCollisionPointRec(ui.mouse, ui.overlay[i])) inside = true;
    if (res >= 0) m->open = false;
    else if (!m->fresh && (ui.pressed || ui.rpressed) && !inside) { m->open = false; ui.click_used = true; }
    else if (IsKeyPressed(KEY_ESCAPE)) m->open = false;
    m->fresh = false;
    ui.layer = saved;
    return res;
}

/* ---- formatting ------------------------------------------------------------------------------ */

void fmt_bytes(double v, char* out, int n)
{
    const char* u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    int i = 0;
    while (v >= 1024 && i < 5) { v /= 1024; ++i; }
    if (i == 0) snprintf(out, n, "%.0f B", v);
    else snprintf(out, n, v >= 100 ? "%.0f %s" : v >= 10 ? "%.1f %s" : "%.2f %s", v, u[i]);
}

void fmt_rate(double v, char* out, int n)
{
    if (v < 1) { snprintf(out, n, "0 B/s"); return; }
    char b[32];
    fmt_bytes(v, b, sizeof b);
    snprintf(out, n, "%s/s", b);
}

void fmt_bits(double bytes_per_s, char* out, int n)
{
    double v = bytes_per_s * 8;
    const char* u[] = { "bps", "Kbps", "Mbps", "Gbps", "Tbps" };
    int i = 0;
    while (v >= 1000 && i < 4) { v /= 1000; ++i; }
    if (i == 0) snprintf(out, n, "%.0f %s", v, u[i]);
    else snprintf(out, n, v >= 100 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

void fmt_dur(double secs, char* out, int n)
{
    if (secs < 0) secs = 0;
    long long s = (long long)secs;
    long long d = s / 86400; s %= 86400;
    int h = (int)(s / 3600), m = (int)(s % 3600 / 60), sec = (int)(s % 60);
    if (d) snprintf(out, n, "%lldd %d:%02d:%02d", d, h, m, sec);
    else if (h) snprintf(out, n, "%d:%02d:%02d", h, m, sec);
    else snprintf(out, n, "%d:%02d", m, sec);
}

void fmt_count(double v, char* out, int n)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%.0f", v);
    int len = (int)strlen(tmp), o = 0;
    for (int i = 0; i < len && o < n - 1; ++i) {
        out[o++] = tmp[i];
        int left = len - i - 1;
        if (left > 0 && left % 3 == 0 && tmp[i] != '-' && o < n - 1) out[o++] = ',';
    }
    out[o] = 0;
}

float nice_ceil(float v)
{
    if (v <= 0) return 1;
    float e = powf(10, floorf(log10f(v)));
    float f = v / e;
    float nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5f ? 2.5f : f <= 5 ? 5 : 10;
    return nf * e;
}

void ui_format_value(int fmt, double v, char* out, int n)
{
    switch (fmt) {
    case GF_PCT:   snprintf(out, n, "%.0f%%", v); break;
    case GF_BYTES: fmt_bytes(v, out, n); break;
    case GF_RATE:  fmt_rate(v, out, n); break;
    case GF_BITS:  fmt_bits(v, out, n); break;
    case GF_TEMP:  snprintf(out, n, "%.0f \xC2\xB0" "C", v); break;
    default:       snprintf(out, n, v >= 100 ? "%.0f" : "%.1f", v); break;
    }
}

/* ---- time series --------------------------------------------------------------------------- */

uint64_t tl_left(void)
{
    uint64_t w = (uint64_t)TL.window;
    return TL.latest + 1 >= w ? TL.latest + 1 - w : 0;
}

static float sample_x(Rectangle r, uint64_t k)
{
    float span = (float)(TL.window - 1);
    float rel = (float)((double)k - ((double)TL.latest - span));
    return r.x + r.width * rel / span;
}

static void fill_under(const Vector2* pts, int n, float base, Color top)
{
    Color bot = col_alpha(top, 0.0f);
    rlBegin(RL_TRIANGLES);
    for (int i = 0; i + 1 < n; ++i) {
        Vector2 a = pts[i], b = pts[i + 1];
        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(a.x, base);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);
        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);
        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(b.x, b.y);
    }
    rlEnd();
}

static Vector2 g_pts[HIST_LEN + 2];

/* Build the polyline for one series inside r. Returns the point count. */
static int series_points(Rectangle r, const Series* s, float maxv, uint64_t* first_k)
{
    uint64_t left = tl_left();
    uint64_t a = s->first > left ? s->first : left;
    uint64_t b = s->last < TL.latest ? s->last : TL.latest;
    if (TL.count == 0 || a > b) return 0;
    *first_k = a;
    int n = 0;
    for (uint64_t k = a; k <= b && n < HIST_LEN; ++k) {
        float v = hist_at(s->ring, k) / maxv;
        if (v > 1) v = 1;
        if (v < 0) v = 0;
        g_pts[n++] = (Vector2){ sample_x(r, k), r.y + r.height - v * r.height };
    }
    return n;
}

float ui_graph(Rectangle r, const Series* s, int ns, const GraphOpts* o)
{
    float maxv = o->maxv;
    if (maxv <= 0) {
        float m = o->minmax > 0 ? o->minmax : 1;
        uint64_t left = tl_left();
        for (int i = 0; i < ns; ++i) {
            uint64_t a = s[i].first > left ? s[i].first : left;
            uint64_t b = s[i].last < TL.latest ? s[i].last : TL.latest;
            for (uint64_t k = a; TL.count && k <= b; ++k) m = fmaxf(m, hist_at(s[i].ring, k));
        }
        maxv = nice_ceil(m * 1.08f);
    }

    /* frame & grid */
    Color gridc = col_alpha(T.border, T.dark ? 0.75f : 0.9f);
    if (!o->mini) {
        ui_rrect(r, 6, T.dark ? col_alpha(BLACK, 0.18f) : col_alpha(T.bg, 0.6f));
        if (o->grid) {
            for (int i = 1; i < 4; ++i) {
                float y = r.y + r.height * i / 4.0f;
                DrawLineEx((Vector2){ r.x, y }, (Vector2){ r.x + r.width, y }, 1.0f / ui.scale, gridc);
            }
            /* a vertical line every minute, anchored on real sample times */
            int step = TL.window <= 60 ? 10 : 60;
            for (uint64_t k = tl_left(); k <= TL.latest && TL.count; ++k) {
                int64_t t = TL.times[k % HIST_LEN] / 1000;
                if (t % step == 0) {
                    float x = sample_x(r, k);
                    DrawLineEx((Vector2){ x, r.y }, (Vector2){ x, r.y + r.height }, 1.0f / ui.scale, gridc);
                }
            }
        }
    }

    ui_scissor_push(r);
    for (int i = 0; i < ns; ++i) {
        uint64_t fk = 0;
        int n = series_points(r, &s[i], maxv, &fk);
        if (n < 1) continue;
        if (n == 1) { g_pts[1] = g_pts[0]; g_pts[1].x += 1; n = 2; }
        if (s[i].fill) fill_under(g_pts, n, r.y + r.height, col_alpha(s[i].color, T.dark ? 0.38f : 0.28f));
        DrawSplineLinear(g_pts, n, o->mini ? 1.25f : 1.6f, s[i].color);
    }
    ui_scissor_pop();

    /* pinned moment */
    if (TL.pin >= 0 && (uint64_t)TL.pin >= tl_left() && (uint64_t)TL.pin <= TL.latest) {
        float x = sample_x(r, (uint64_t)TL.pin);
        DrawLineEx((Vector2){ x, r.y }, (Vector2){ x, r.y + r.height }, 1.5f, col_alpha(T.warn, 0.9f));
    }

    if (!o->mini) {
        ui_rrect_line(r, 6, 1, T.border);
        if (o->axis_labels) {
            char b[32];
            ui_format_value(o->fmt, maxv, b, sizeof b);
            ui_text_fit((Rectangle){ r.x + 6, r.y + 2, r.width - 12, 16 }, b, 11, FW_REG, T.faint, AL_RIGHT);
        }
        if (o->title)
            ui_text_fit((Rectangle){ r.x + 8, r.y + 2, r.width - 16, 18 }, o->title, 11.5f, FW_SEMI, T.dim, AL_LEFT);
    }

    /* crosshair: hovering any graph moves the shared cursor for all of them */
    bool hov = !o->mini && ui_hover(r);
    if (hov && TL.count) {
        float rel = (ui.mouse.x - r.x) / r.width;
        double kf = (double)TL.latest - (TL.window - 1) * (1.0 - rel);
        int64_t k = (int64_t)llround(kf);
        if (k < (int64_t)tl_left()) k = (int64_t)tl_left();
        if (k > (int64_t)TL.latest) k = (int64_t)TL.latest;
        TL.hover_next = k;
        if (ui_clicked(r)) TL.pin = (TL.pin == k) ? -1 : k;
    }
    if (TL.hover >= (int64_t)tl_left() && TL.hover <= (int64_t)TL.latest && TL.count) {
        uint64_t k = (uint64_t)TL.hover;
        float x = sample_x(r, k);
        DrawLineEx((Vector2){ x, r.y }, (Vector2){ x, r.y + r.height }, 1.0f, col_alpha(T.text, 0.35f));
        for (int i = 0; i < ns; ++i) {
            if (k < s[i].first || k > s[i].last) continue;
            float v = fminf(1, fmaxf(0, hist_at(s[i].ring, k) / maxv));
            DrawCircleV((Vector2){ x, r.y + r.height - v * r.height }, o->mini ? 2.5f : 3.5f, s[i].color);
        }
        if (hov && o->tooltip) {
            char tip[512], clock[16], val[32];
            sys_format_clock(TL.times[k % HIST_LEN], clock, sizeof clock);
            char ago[24];
            fmt_dur((double)(TL.latest - k), ago, sizeof ago);
            int off = snprintf(tip, sizeof tip, "%s  (-%s)", clock, ago);
            for (int i = 0; i < ns && off < (int)sizeof tip - 64; ++i) {
                if (k < s[i].first || k > s[i].last) continue;
                ui_format_value(o->fmt, hist_at(s[i].ring, k), val, sizeof val);
                off += snprintf(tip + off, sizeof tip - off, "\n%s: %s", s[i].label ? s[i].label : "Value", val);
            }
            ui_tooltip(tip);
        }
    }
    return maxv;
}

void ui_sparkline(Rectangle r, const float* ring, uint64_t first, uint64_t last, float maxv, Color c)
{
    Series s = { ring, first, last, c, NULL, true, false };
    GraphOpts o = { .maxv = maxv > 0 ? maxv : 0, .minmax = 1, .mini = true };
    ui_graph(r, &s, 1, &o);
}
