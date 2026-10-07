#ifndef TARMAN_UI_H
#define TARMAN_UI_H

/* Immediate-mode widget layer on raylib.
 *
 * Coordinates: everything is laid out in logical pixels. The frame is drawn
 * inside a Camera2D whose zoom is the UI scale, and fonts are baked at
 * size * scale, so a glyph texel lands on exactly one screen pixel -- text
 * stays crisp at 100%, 150% or 225% without the layout code knowing.
 *
 * Input layering: menus and modals register their rectangles as overlays;
 * on the next frame the base layer treats the mouse as absent wherever an
 * overlay sits, so a click on a menu never also lands on the row under it. */

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h"

/* ---- theme --------------------------------------------------------------- */

typedef struct {
    Color bg, side, panel, card, hover, press, border, line;
    Color text, dim, faint, accent, on_accent;
    Color cpu, mem, disk, net, gpu, kernel;
    Color good, warn, bad, sel;
    bool  dark;
} Theme;

extern Theme T;
void ui_theme(bool dark);

Color col_alpha(Color c, float a);
Color col_mix(Color a, Color b, float t);
Color ui_heat(float t);                     /* 0..1 -> cell tint, Task Manager style */

/* ---- glyphs (Segoe Fluent Icons / MDL2 Assets codepoints) ------------------ */

enum {
    IC_MENU = 0xE700, IC_PROCESSES = 0xE71D, IC_PERF = 0xE9D2, IC_HISTORY = 0xE81C,
    IC_GLOBE = 0xE774, IC_DISK = 0xEDA2, IC_CHIP = 0xE950, IC_BOLT = 0xE945,
    IC_PEOPLE = 0xE716, IC_PERSON = 0xE77B, IC_SERVICES = 0xE9F5, IC_SETTINGS = 0xE713,
    IC_SEARCH = 0xE721, IC_PAUSE = 0xE769, IC_PLAY = 0xE768, IC_CANCEL = 0xE711,
    IC_ADD = 0xE710, IC_RUN = 0xE8A7, IC_SHIELD = 0xEA18, IC_CHEV_R = 0xE76C,
    IC_CHEV_D = 0xE70D, IC_CHEV_U = 0xE70E, IC_FOLDER = 0xE8B7, IC_COPY = 0xE8C8,
    IC_INFO = 0xE946, IC_PIN = 0xE718, IC_REFRESH = 0xE72C, IC_MONITOR = 0xE7F4,
    IC_WIFI = 0xE701, IC_ETHERNET = 0xE839, IC_CLOCK = 0xE823, IC_WARNING = 0xE7BA,
    IC_DOWNLOAD = 0xE896, IC_SORT = 0xE8CB, IC_SUN = 0xE706, IC_MOON = 0xE708,
    IC_LEAF = 0xE8BE, IC_CHECK = 0xE73E, IC_LIST = 0xE8FD, IC_TREE = 0xE8A4,
    IC_STOP = 0xE71A, IC_DELETE = 0xE74D, IC_LINK = 0xE71B, IC_POWER = 0xE7E8,
    IC_MEMORY = 0xE964, IC_THERMO = 0xE9CA, IC_SHIELD_WIN = 0xE7EF,
    IC_WIN_MIN = 0xE921, IC_WIN_MAX = 0xE922, IC_WIN_RESTORE = 0xE923, IC_WIN_CLOSE = 0xE8BB,
};

/* ---- fonts & text ---------------------------------------------------------- */

typedef enum { FW_REG = 0, FW_SEMI, FW_BOLD, FW_MONO, FW_ICON, FW_COUNT } FontWeight;
typedef enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT } Align;

void  ui_fonts_init(void);
void  ui_fonts_free(void);
float ui_text_w(const char* s, float size, int weight);
void  ui_text(float x, float y, const char* s, float size, int weight, Color c);
/* Draws `s` vertically centred in `r`, ellipsised to fit its width. */
void  ui_text_fit(Rectangle r, const char* s, float size, int weight, Color c, int align);
void  ui_icon(float cx, float cy, int codepoint, float size, Color c);

/* ---- primitives -------------------------------------------------------------- */

void ui_rrect(Rectangle r, float radius, Color c);
void ui_rrect_line(Rectangle r, float radius, float thick, Color c);
void ui_vgradient(Rectangle r, Color top, Color bottom);
void ui_scissor_push(Rectangle r);
void ui_scissor_pop(void);
Rectangle rect_inset(Rectangle r, float dx, float dy);
Rectangle rect_cut_left(Rectangle* r, float w);
Rectangle rect_cut_right(Rectangle* r, float w);
Rectangle rect_cut_top(Rectangle* r, float h);
Rectangle rect_cut_bottom(Rectangle* r, float h);

/* ---- frame & input -------------------------------------------------------------- */

#define UI_MAX_OVERLAYS 8

typedef struct {
    float     scale;
    float     w, h;                 /* logical window size        */
    Vector2   mouse;                /* logical                    */
    bool      down, pressed, released, rpressed, dbl;
    float     wheel;
    double    time;
    bool      click_used, wheel_used, rclick_used;
    int       layer;                /* 0 base, 1 overlay          */
    Rectangle overlay[UI_MAX_OVERLAYS], overlay_prev[UI_MAX_OVERLAYS];
    int       noverlay, noverlay_prev;
    bool      modal, modal_prev;
    uint32_t  active;               /* widget being dragged       */
    uint32_t  focus;                /* text box with keyboard     */
    uint32_t  focus_next;
    int       cursor;               /* MouseCursor for this frame */
    char      tip[512];
    Vector2   tip_at;
    double    last_click_time;
    bool      no_input;             /* --screenshot: ignore mouse and keys */
    Vector2   last_click_pos;
} UI;

extern UI ui;

void ui_set_scale(float scale);
void ui_frame_begin(void);
void ui_frame_end(void);           /* tooltip + cursor */
void ui_overlay(Rectangle r);      /* register an overlay rect for input blocking */
bool ui_input_ok(void);            /* mouse may interact with the current layer */
bool ui_hover(Rectangle r);
bool ui_clicked(Rectangle r);
bool ui_rclicked(Rectangle r);
bool ui_dclicked(Rectangle r);
bool ui_wheel(Rectangle r, float* scroll, float step);
void ui_tooltip(const char* s);
uint32_t ui_id(const char* s, int n);

/* ---- widgets ------------------------------------------------------------------- */

typedef enum { BTN_SUBTLE = 0, BTN_PRIMARY, BTN_DANGER, BTN_GHOST } ButtonStyle;

float ui_button_w(const char* label, int icon);
bool  ui_button(Rectangle r, const char* label, int icon, int style, bool enabled);
bool  ui_icon_button(Rectangle r, int icon, const char* tip, bool active);
bool  ui_segmented(Rectangle r, const char* const* items, int n, int* sel);
bool  ui_checkbox(Rectangle r, const char* label, bool* v);
bool  ui_switch(Rectangle r, bool* v);
bool  ui_textbox(Rectangle r, char* buf, int n, uint32_t id, const char* placeholder, int icon);
void  ui_scrollbar(Rectangle track, float* scroll, float content, float view, uint32_t id);
void  ui_bar(Rectangle r, float t, Color c);   /* thin progress bar */
void  ui_badge(float x, float cy, const char* s, Color c);

/* ---- menus ---------------------------------------------------------------------- */

#define MENU_MAX 48

typedef struct {
    char label[64];
    int  icon, id;
    bool enabled, checked, sep;
    int  parent;                 /* -1 root, else index of submenu owner */
    bool has_sub;
} MenuItem;

typedef struct {
    bool     open;
    Vector2  pos;
    MenuItem items[MENU_MAX];
    int      n;
    int      sub_open;           /* index of the open submenu owner, -1 none */
    int      owner;              /* who opened it (page-defined) */
    uint32_t key_a;              /* payload, e.g. pid */
    uint64_t key_b;
    bool     fresh;
} Menu;

void menu_open(Menu* m, Vector2 at, int owner);
int  menu_add(Menu* m, const char* label, int icon, int id, bool enabled, bool checked);
int  menu_add_sub(Menu* m, const char* label, int icon);   /* returns parent index */
int  menu_add_child(Menu* m, int parent, const char* label, int id, bool enabled, bool checked);
void menu_sep(Menu* m);
int  menu_run(Menu* m);          /* draw (overlay layer); returns clicked id or -1 */

/* ---- formatting -------------------------------------------------------------------- */

void  fmt_bytes(double v, char* out, int n);       /* 1.2 GB          */
void  fmt_rate(double v, char* out, int n);        /* 1.2 MB/s        */
void  fmt_bits(double bytes_per_s, char* out, int n); /* 9.6 Mbps     */
void  fmt_dur(double secs, char* out, int n);      /* 1:02:03, 2d 4:05:06 */
void  fmt_count(double v, char* out, int n);       /* 12,345          */
float nice_ceil(float v);

/* ---- time series ------------------------------------------------------------------- */

typedef enum { GF_PCT = 0, GF_BYTES, GF_RATE, GF_BITS, GF_NUM, GF_TEMP } GraphFormat;

typedef struct {
    const float* ring;           /* HIST_LEN ring, aligned on the global sample clock */
    uint64_t     first, last;    /* valid sample range */
    Color        color;
    const char*  label;
    bool         fill;
    bool         dashed;
} Series;

typedef struct {
    float maxv;                  /* fixed max; <= 0 means auto (nice ceiling) */
    float minmax;                /* auto-max floor                            */
    int   fmt;
    bool  grid, axis_labels, tooltip, mini;
    const char* title;
} GraphOpts;

/* The shared clock: set once per frame by the app. */
typedef struct {
    uint64_t       latest;       /* newest sample index (samples - 1) */
    uint64_t       count;        /* samples recorded */
    int            window;       /* visible seconds (60, 300, 600)   */
    int64_t        hover;        /* sample under the cursor last frame, -1 none */
    int64_t        hover_next;
    int64_t        pin;          /* pinned sample, -1 none */
    const int64_t* times;        /* sample_time ring */
} Timeline;

extern Timeline TL;

uint64_t tl_left(void);          /* oldest visible sample */
/* Draws a graph; returns the max value used for the y axis. */
float ui_graph(Rectangle r, const Series* s, int ns, const GraphOpts* o);
void  ui_sparkline(Rectangle r, const float* ring, uint64_t first, uint64_t last, float maxv, Color c);
void  ui_format_value(int fmt, double v, char* out, int n);

#endif /* TARMAN_UI_H */
