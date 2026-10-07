/* Tarman -- a task manager and resource monitor for Windows, drawn with
 * raylib. main.c owns the window, the frame loop, the sidebar and status bar,
 * modal dialogs, settings persistence and the command line; the pages live
 * in page_*.c and the sampler in sys_win.c. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "raylib.h"
#include "rlgl.h"
#include "version.h"

App app;

static const int WINDOWS_SEC[3] = { 60, 300, 600 };

/* ---- settings ---------------------------------------------------------------- */

static int g_win_w = 1380, g_win_h = 880;

static void settings_load(void)
{
    char path[512];
    if (!sys_settings_path(path, sizeof path)) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        float v;
        if (sscanf(line, "%63[^=]=%f", key, &v) != 2) continue;
        if (!strcmp(key, "dark")) app.dark = v != 0;
        else if (!strcmp(key, "zoom")) app.zoom = v;
        else if (!strcmp(key, "topmost")) app.topmost = v != 0;
        else if (!strcmp(key, "window")) app.window_idx = (int)v;
        else if (!strcmp(key, "page")) app.page = (int)v;
        else if (!strcmp(key, "nav_collapsed")) app.nav_collapsed = v != 0;
        else if (!strcmp(key, "proc_view")) app.proc_view = (int)v;
        else if (!strcmp(key, "hoswl")) app.hoswl = v != 0;
        else if (!strcmp(key, "win_w")) g_win_w = (int)v;
        else if (!strcmp(key, "win_h")) g_win_h = (int)v;
    }
    fclose(f);
    if (app.window_idx < 0 || app.window_idx > 2) app.window_idx = 2;
    if (app.page < 0 || app.page >= PAGE_COUNT) app.page = 0;
    if (app.proc_view < 0 || app.proc_view > 2) app.proc_view = 0;
    if (app.zoom < 0.75f || app.zoom > 2.0f) app.zoom = 1.0f;
    if (g_win_w < 900 || g_win_w > 8000) g_win_w = 1380;
    if (g_win_h < 600 || g_win_h > 8000) g_win_h = 880;
}

void settings_save(void)
{
    char path[512];
    if (!sys_settings_path(path, sizeof path)) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    int w = IsWindowReady() ? (int)(GetScreenWidth() / ui.scale) : g_win_w;
    int h = IsWindowReady() ? (int)(GetScreenHeight() / ui.scale) : g_win_h;
    fprintf(f, "dark=%d\nzoom=%.2f\ntopmost=%d\nwindow=%d\npage=%d\nnav_collapsed=%d\nproc_view=%d\nwin_w=%d\nwin_h=%d\nhoswl=%d\n",
            app.dark, app.zoom, app.topmost, app.window_idx, app.page, app.nav_collapsed, app.proc_view, w, h, app.hoswl);
    fclose(f);
}

/* ---- the logo ---------------------------------------------------------------------- */

/* tools/make-logo.ps1 renders the mark once and writes it everywhere: the .ico
 * compiled into the exe (and used by the installer), and these PNGs, so the
 * window icon, title bar and About box can never drift from it. */
#include "logo_png.h"

static Image logo_image(int px)
{
    const unsigned char* data = px <= 32 ? LOGO_PNG_32 : px <= 64 ? LOGO_PNG_64 : LOGO_PNG_256;
    int len = px <= 32 ? (int)sizeof LOGO_PNG_32 : px <= 64 ? (int)sizeof LOGO_PNG_64 : (int)sizeof LOGO_PNG_256;
    return LoadImageFromMemory(".png", data, len);
}

/* The smallest embedded frame that covers `logical` px at the current scale,
 * so the title bar draws a natively rendered 32 px mark, not a blurred 256. */
static Texture2D logo_texture(float logical)
{
    static Texture2D tex[3];
    static const int sizes[3] = { 32, 64, 256 };
    float need = logical * ui.scale;
    int k = need <= 32 ? 0 : need <= 64 ? 1 : 2;
    if (!tex[k].id) {
        Image im = logo_image(sizes[k]);
        tex[k] = LoadTextureFromImage(im);
        GenTextureMipmaps(&tex[k]);
        SetTextureFilter(tex[k], TEXTURE_FILTER_TRILINEAR);
        UnloadImage(im);
    }
    return tex[k];
}

static void draw_logo(float x, float y, float size, Color tint)
{
    Texture2D t = logo_texture(size);
    DrawTexturePro(t, (Rectangle){ 0, 0, (float)t.width, (float)t.height }, (Rectangle){ x, y, size, size },
                   (Vector2){ 0 }, 0, tint);
}

/* ---- sidebar ---------------------------------------------------------------------- */

typedef struct { int page; const char* label; int icon; } NavItem;

static const NavItem NAV[] = {
    { PAGE_PROCESSES, "Processes", IC_PROCESSES },
    { PAGE_PERFORMANCE, "Performance", IC_PERF },
    { PAGE_ANALYSIS, "Analysis", IC_HISTORY },
    { -1, "Resource monitor", 0 },
    { PAGE_NETWORK, "Network", IC_GLOBE },
    { PAGE_DISK, "Disk", IC_DISK },
    { PAGE_MEMORY, "Memory", IC_MEMORY },
    { -1, "System", 0 },
    { PAGE_STARTUP, "Startup apps", IC_BOLT },
    { PAGE_USERS, "Users", IC_PEOPLE },
    { PAGE_SERVICES, "Services", IC_SERVICES },
    { PAGE_EVENTS, "Events", IC_LIST },
    { PAGE_FIRMWARE, "Firmware", IC_CHIP },
};

static void nav_item(Rectangle r, int page, const char* label, int icon, bool collapsed)
{
    bool sel = app.page == page;
    bool hov = ui_hover(r);
    if (sel) ui_rrect(r, 7, T.dark ? T.hover : T.press);
    else if (hov) ui_rrect(r, 7, col_alpha(T.hover, T.dark ? 0.7f : 1.0f));
    if (sel) ui_rrect((Rectangle){ r.x, r.y + 10, 3, r.height - 20 }, 1.5f, T.accent);
    float ix = collapsed ? r.x + r.width / 2 : r.x + 24;
    ui_icon(ix, r.y + r.height / 2, icon, 16, sel ? T.accent : (hov ? T.text : T.dim));
    if (!collapsed) ui_text_fit((Rectangle){ r.x + 48, r.y, r.width - 56, r.height }, label, 13.5f,
                                sel ? FW_SEMI : FW_REG, sel ? T.text : T.dim, AL_LEFT);
    if (hov) {
        ui.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (collapsed) ui_tooltip(label);
    }
    if (ui_clicked(r) && app.page != page) { app.page = page; settings_save(); }
}

static void mini_meter(Rectangle r, const char* label, float t, const char* value, Color c)
{
    ui_text_fit((Rectangle){ r.x, r.y, 60, 16 }, label, 11.5f, FW_REG, T.dim, AL_LEFT);
    ui_text_fit((Rectangle){ r.x + 60, r.y, r.width - 60, 16 }, value, 11.5f, FW_SEMI, T.text, AL_RIGHT);
    ui_bar((Rectangle){ r.x, r.y + 19, r.width, 4 }, t, c);
}

static void draw_sidebar(Rectangle r)
{
    bool col = app.nav_collapsed;
    DrawRectangleRec(r, T.side);
    DrawRectangleRec((Rectangle){ r.x + r.width - 1, r.y, 1, r.height }, T.border);
    Rectangle in = rect_inset(r, 8, 10);

    Rectangle top = rect_cut_top(&in, 40);
    if (ui_icon_button((Rectangle){ top.x + (col ? (top.width - 36) / 2 : 6), top.y + 2, 36, 36 }, IC_MENU,
                       col ? "Expand" : "Collapse", false)) {
        app.nav_collapsed = !col;
        settings_save();
    }
    rect_cut_top(&in, 8);

    for (size_t i = 0; i < sizeof NAV / sizeof NAV[0]; ++i) {
        if (NAV[i].page < 0) {
            Rectangle s = rect_cut_top(&in, col ? 14 : 30);
            if (col) DrawRectangleRec((Rectangle){ s.x + 12, s.y + 6, s.width - 24, 1 }, T.border);
            else ui_text_fit((Rectangle){ s.x + 14, s.y + 8, s.width - 20, 20 }, NAV[i].label, 11, FW_SEMI, T.faint, AL_LEFT);
            continue;
        }
        nav_item(rect_cut_top(&in, 40), NAV[i].page, NAV[i].label, NAV[i].icon, col);
        rect_cut_top(&in, 2);
    }

    Rectangle bottom = rect_cut_bottom(&in, 42);
    nav_item(bottom, PAGE_SETTINGS, "Settings", IC_SETTINGS, col);

    if (!col && in.height > 150) {
        SysState* st = app.st;
        Rectangle m = rect_cut_bottom(&in, 140);
        m = rect_inset(m, 12, 4);
        char v[48], a[32], b[32];
        snprintf(v, sizeof v, "%.0f%%", st->cpu.usage);
        mini_meter(rect_cut_top(&m, 32), "CPU", st->cpu.usage / 100, v, T.cpu);
        fmt_bytes((double)st->mem.used, a, sizeof a);
        snprintf(v, sizeof v, "%s", a);
        mini_meter(rect_cut_top(&m, 32), "Memory", st->mem.load / 100, v, T.mem);
        float disk = 0;
        for (int i = 0; i < st->ndisk; ++i) disk = fmaxf(disk, st->disk[i].active);
        snprintf(v, sizeof v, "%.0f%%", disk);
        mini_meter(rect_cut_top(&m, 32), "Disk", disk / 100, v, T.disk);
        double net = 0, link = 0;
        for (int i = 0; i < st->nnet; ++i) { net += st->net[i].recv_bps + st->net[i].send_bps; link += st->net[i].link_bps / 8.0; }
        fmt_bits(net, b, sizeof b);
        mini_meter(rect_cut_top(&m, 32), "Network", link > 0 ? (float)(net / link) * 20 : 0, b, T.net);
    }
}

/* ---- title bar ---------------------------------------------------------------------- */

#define TITLE_H 34

static void caption_button(Rectangle b, int glyph, bool hov, bool down, bool close, bool active)
{
    Color bg = BLANK, fg = active ? T.text : T.faint;
    if (close && hov) { bg = down ? (Color){ 0x94, 0x1E, 0x14, 255 } : (Color){ 0xC4, 0x2B, 0x1C, 255 }; fg = WHITE; }
    else if (hov) bg = down ? T.press : T.hover;
    if (bg.a) DrawRectangleRec(b, bg);
    ui_icon(b.x + b.width / 2, b.y + b.height / 2, glyph, 10, fg);
}

static void draw_titlebar(Rectangle r)
{
    bool active = sys_window_active();
    DrawRectangleRec(r, T.side);
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 1, r.width, 1 }, T.border);

    draw_logo(r.x + 12, r.y + (r.height - 20) / 2, 20, active ? WHITE : col_alpha(WHITE, 0.65f));

    const float bw = 46;
    Rectangle bclose = { r.x + r.width - bw, r.y, bw, r.height - 1 };
    Rectangle bmax   = { bclose.x - bw, r.y, bw, r.height - 1 };
    Rectangle bmin   = { bmax.x - bw, r.y, bw, r.height - 1 };

    /* centred title; elevated sessions say so, like an admin console */
    char t[96];
    snprintf(t, sizeof t, "%s", app.st->elevated ? "Tarman  \xC2\xB7  Administrator" : "Tarman");
    ui_text_fit((Rectangle){ r.x + bw * 3, r.y, r.width - bw * 6, r.height - 1 }, t, 12.5f, FW_SEMI,
                active ? T.text : T.faint, AL_CENTER);

    /* hand Windows the geometry: caption height, the snap-layout maximize
     * button and the buttons we handle ourselves */
    float s = ui.scale;
    int maxr[4] = { (int)(bmax.x * s), (int)(bmax.y * s), (int)ceilf(bmax.width * s), (int)ceilf(bmax.height * s) };
    int cl[2][4] = {
        { (int)(bmin.x * s), (int)(bmin.y * s), (int)ceilf(bmin.width * s), (int)ceilf(bmin.height * s) },
        { (int)(bclose.x * s), (int)(bclose.y * s), (int)ceilf(bclose.width * s) + 2, (int)ceilf(bclose.height * s) },
    };
    sys_titlebar_update((int)ceilf(r.height * s), maxr, (const int (*)[4])cl, 2);

    bool hmin = ui_hover(bmin), hclose = ui_hover(bclose);
    caption_button(bmin, IC_WIN_MIN, hmin, hmin && ui.down, false, active);
    caption_button(bmax, IsWindowMaximized() ? IC_WIN_RESTORE : IC_WIN_MAX, sys_titlebar_max_hover(),
                   sys_titlebar_max_down(), false, active);
    caption_button(bclose, IC_WIN_CLOSE, hclose, hclose && ui.down, true, active);
    if (hmin) ui_tooltip("Minimize");
    if (hclose) ui_tooltip("Close");
    if (ui_clicked(bmin)) MinimizeWindow();
    if (ui_clicked(bclose)) app.quit = true;
}

/* ---- status bar ------------------------------------------------------------------------- */

static void draw_statusbar(Rectangle r)
{
    SysState* st = app.st;
    DrawRectangleRec(r, T.side);
    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, 1 }, T.border);
    Rectangle in = rect_inset(r, 14, 0);

    /* right: window zoom, pause, elevation */
    if (!st->elevated) {
        float w = ui_button_w("Run as admin", IC_SHIELD) - 4;
        Rectangle b = rect_cut_right(&in, w);
        if (ui_button((Rectangle){ b.x, r.y + 4, b.width, r.height - 8 }, "Run as admin", IC_SHIELD, BTN_GHOST, true))
            if (sys_restart_elevated()) app.quit = true;
        rect_cut_right(&in, 6);
    }
    Rectangle pb = rect_cut_right(&in, 30);
    if (ui_icon_button((Rectangle){ pb.x, r.y + 3, 30, r.height - 6 }, app.paused ? IC_PLAY : IC_PAUSE,
                       app.paused ? "Resume recording (Space)" : "Pause recording (Space)", app.paused)) {
        app.paused = !app.paused;
        sys_set_paused(app.paused);
    }
    rect_cut_right(&in, 8);
    static const char* wins[] = { "1m", "5m", "10m" };
    Rectangle seg = rect_cut_right(&in, 132);
    if (ui_segmented((Rectangle){ seg.x, r.y + 4, seg.width, r.height - 8 }, wins, 3, &app.window_idx)) settings_save();
    rect_cut_right(&in, 8);
    ui_text_fit(rect_cut_right(&in, 46), "Window", 12, FW_REG, T.faint, AL_RIGHT);

    /* left: recording state */
    Color dot = app.paused ? T.warn : T.good;
    float pulse = app.paused ? 1.0f : 0.55f + 0.45f * (float)fabs(sin(ui.time * 2.2));
    DrawCircleV((Vector2){ in.x + 5, r.y + r.height / 2 }, 4, col_alpha(dot, pulse));
    char rec[96], d[24];
    uint64_t have = TL.count < HIST_LEN ? TL.count : HIST_LEN;
    fmt_dur((double)have, d, sizeof d);
    snprintf(rec, sizeof rec, "%s  \xC2\xB7  %s of 10:00 buffered", app.paused ? "Paused" : "Recording", d);
    float rw = ui_text_w(rec, 12, FW_REG) + 6;
    ui_text_fit((Rectangle){ in.x + 16, r.y, rw, r.height }, rec, 12, FW_REG, T.dim, AL_LEFT);
    in.x += rw + 30;
    in.width -= rw + 30;

    char s[256], a[32], u[32];
    fmt_dur(st->uptime_ms / 1000.0, u, sizeof u);
    fmt_bytes((double)st->mem.used, a, sizeof a);
    snprintf(s, sizeof s, "CPU %.0f%%   \xC2\xB7   Memory %s (%.0f%%)   \xC2\xB7   %d processes   \xC2\xB7   %d threads   \xC2\xB7   Up %s",
             st->cpu.usage, a, st->mem.load, st->total_procs, st->total_threads, u);
    ui_text_fit(in, s, 12, FW_REG, T.faint, AL_LEFT);
}

/* ---- modals ---------------------------------------------------------------------------------- */

void open_run_dialog(void)
{
    app.modal = MODAL_RUN;
    app.run_admin = false;
    ui.focus_next = ui_id("run-cmd", 0);
}

void open_affinity_dialog(const Proc* p)
{
    ProcExtra ex;
    sys_proc_extra(p->pid, &ex);
    if (!ex.ok) { toast(true, "Couldn't read affinity for %s (access denied)", p->name); return; }
    app.modal = MODAL_AFFINITY;
    app.modal_pid = p->pid;
    app.modal_create = p->create_time;
    snprintf(app.modal_name, sizeof app.modal_name, "%s", p->name);
    app.aff_mask = ex.affinity;
    app.aff_sys = ex.sys_affinity;
}

static Rectangle modal_frame(float w, float h, const char* title)
{
    ui.modal = true;
    ui.layer = 1;
    DrawRectangleRec((Rectangle){ 0, 0, ui.w, ui.h }, col_alpha(BLACK, T.dark ? 0.55f : 0.3f));
    Rectangle r = { floorf((ui.w - w) / 2), floorf((ui.h - h) / 2), w, h };
    ui_rrect((Rectangle){ r.x, r.y + 6, r.width, r.height }, 12, col_alpha(BLACK, 0.35f));
    ui_rrect(r, 12, T.dark ? (Color){ 0x18, 0x1E, 0x28, 0xFF } : T.panel);
    ui_rrect_line(r, 12, 1, T.border);
    ui_text_fit((Rectangle){ r.x + 24, r.y + 18, r.width - 48, 30 }, title, 17, FW_SEMI, T.text, AL_LEFT);
    return (Rectangle){ r.x + 24, r.y + 58, r.width - 48, r.height - 58 - 20 };
}

static void confirm_run(void)
{
    int a = app.modal_action;
    if (a == PA_SES_SIGNOFF) {
        char err[256];
        if (sys_session_action(app.user_sel, 1, err, sizeof err)) toast(false, "Signed off session %u", app.user_sel);
        else toast(true, "Couldn't sign off: %s", err);
    } else {
        proc_action(a, app.modal_pid, app.modal_create);
    }
}

static void draw_modal(void)
{
    if (!app.modal) return;
    bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    bool esc = IsKeyPressed(KEY_ESCAPE);
    switch (app.modal) {
    case MODAL_CONFIRM: {
        Rectangle in = modal_frame(460, 200, app.modal_title);
        Rectangle tr = rect_cut_top(&in, 70);
        /* two-line wrap: split near the middle space */
        const char* t = app.modal_text;
        char l1[200], l2[200];
        snprintf(l1, sizeof l1, "%s", t);
        l2[0] = 0;
        if (ui_text_w(t, 13, FW_REG) > tr.width) {
            size_t n = strlen(t), cut = n / 2;
            while (cut < n && t[cut] != ' ') ++cut;
            snprintf(l1, sizeof l1, "%.*s", (int)cut, t);
            snprintf(l2, sizeof l2, "%s", t + (cut < n ? cut + 1 : n));
        }
        ui_text_fit((Rectangle){ tr.x, tr.y, tr.width, 22 }, l1, 13, FW_REG, T.dim, AL_LEFT);
        ui_text_fit((Rectangle){ tr.x, tr.y + 22, tr.width, 22 }, l2, 13, FW_REG, T.dim, AL_LEFT);
        Rectangle br = rect_cut_bottom(&in, 34);
        const char* go = app.modal_action == PA_SES_SIGNOFF ? "Sign off" : "End process";
        float gw = ui_button_w(go, 0) + 10;
        bool ok = ui_button((Rectangle){ br.x + br.width - gw, br.y, gw, 34 }, go, 0, BTN_DANGER, true);
        bool cancel = ui_button((Rectangle){ br.x + br.width - gw - 100, br.y, 92, 34 }, "Cancel", 0, BTN_SUBTLE, true);
        if (ok || enter) { confirm_run(); app.modal = MODAL_NONE; }
        else if (cancel || esc) app.modal = MODAL_NONE;
        break;
    }
    case MODAL_RUN: {
        Rectangle in = modal_frame(520, 250, "Create new task");
        ui_text_fit(rect_cut_top(&in, 22), "Type the name of a program, folder, document or Internet resource.", 13, FW_REG, T.dim, AL_LEFT);
        rect_cut_top(&in, 10);
        uint32_t id = ui_id("run-cmd", 0);
        ui_textbox(rect_cut_top(&in, 36), app.run_cmd, sizeof app.run_cmd, id, "e.g. notepad, cmd, https://\xE2\x80\xA6", IC_RUN);
        ui.focus_next = id;
        rect_cut_top(&in, 10);
        ui_checkbox(rect_cut_top(&in, 26), "Create this task with administrative privileges", &app.run_admin);
        Rectangle br = rect_cut_bottom(&in, 34);
        bool ok = ui_button((Rectangle){ br.x + br.width - 92, br.y, 92, 34 }, "Run", IC_RUN, BTN_PRIMARY, app.run_cmd[0] != 0);
        bool cancel = ui_button((Rectangle){ br.x + br.width - 192, br.y, 92, 34 }, "Cancel", 0, BTN_SUBTLE, true);
        if ((ok || enter) && app.run_cmd[0]) {
            char err[256];
            if (sys_run(app.run_cmd, app.run_admin, err, sizeof err)) { toast(false, "Started %s", app.run_cmd); app.modal = MODAL_NONE; }
            else toast(true, "Couldn't run %s: %s", app.run_cmd, err);
        } else if (cancel || IsKeyPressed(KEY_ESCAPE)) {
            app.modal = MODAL_NONE;
            ui.focus_next = 0;
        }
        break;
    }
    case MODAL_AFFINITY: {
        int n = 0;
        for (int i = 0; i < 64; ++i) if (app.aff_sys & (1ULL << i)) n = i + 1;
        int cols = n > 16 ? 8 : 4;
        int rows = (n + cols - 1) / cols;
        char title[160];
        snprintf(title, sizeof title, "Processor affinity \xE2\x80\x94 %s", app.modal_name);
        Rectangle in = modal_frame(cols * 96 + 48, 200 + rows * 30, title);
        ui_text_fit(rect_cut_top(&in, 22), "Which processors are allowed to run this process?", 13, FW_REG, T.dim, AL_LEFT);
        rect_cut_top(&in, 8);
        bool all = (app.aff_mask & app.aff_sys) == app.aff_sys;
        if (ui_checkbox(rect_cut_top(&in, 28), "<All processors>", &all)) app.aff_mask = all ? app.aff_sys : 0;
        rect_cut_top(&in, 6);
        Rectangle grid = rect_cut_top(&in, rows * 30);
        for (int i = 0; i < n; ++i) {
            if (!(app.aff_sys & (1ULL << i))) continue;
            bool on = (app.aff_mask >> i) & 1;
            char lab[16];
            snprintf(lab, sizeof lab, "CPU %d", i);
            Rectangle c = { grid.x + (i % cols) * 96, grid.y + (i / cols) * 30, 92, 28 };
            if (ui_checkbox(c, lab, &on)) app.aff_mask = on ? app.aff_mask | (1ULL << i) : app.aff_mask & ~(1ULL << i);
        }
        Rectangle br = rect_cut_bottom(&in, 34);
        bool ok = ui_button((Rectangle){ br.x + br.width - 92, br.y, 92, 34 }, "OK", 0, BTN_PRIMARY, app.aff_mask != 0);
        bool cancel = ui_button((Rectangle){ br.x + br.width - 192, br.y, 92, 34 }, "Cancel", 0, BTN_SUBTLE, true);
        if ((ok || enter) && app.aff_mask) {
            char err[256];
            if (sys_set_affinity(app.modal_pid, app.aff_mask, err, sizeof err)) toast(false, "Affinity updated for %s", app.modal_name);
            else toast(true, "Couldn't set affinity: %s", err);
            app.modal = MODAL_NONE;
            app.extra_time = 0;
        } else if (cancel || esc) app.modal = MODAL_NONE;
        break;
    }
    case MODAL_ABOUT: {
        Rectangle in = modal_frame(480, 340, "About Tarman");
        draw_logo(in.x + in.width - 64, in.y - 46, 64, WHITE);
        char b[160];
        snprintf(b, sizeof b, "Version %s  \xC2\xB7  raylib %s", TARMAN_VERSION, RAYLIB_VERSION);
        ui_text_fit(rect_cut_top(&in, 24), b, 13, FW_REG, T.text, AL_LEFT);
        ui_text_fit(rect_cut_top(&in, 22), "Task manager and resource monitor with a 10-minute moving window.", 13, FW_REG, T.dim, AL_LEFT);
        rect_cut_top(&in, 10);
        const char* keys[] = { "Ctrl+1 \xE2\x80\xA6 Ctrl+9   switch pages", "Ctrl+F   search", "Ctrl+N   run new task",
                               "Delete   end selected task", "Space   pause / resume recording",
                               "Ctrl+Plus / Minus   interface scale" };
        for (int i = 0; i < 6; ++i) ui_text_fit(rect_cut_top(&in, 21), keys[i], 12.5f, FW_REG, T.dim, AL_LEFT);
        Rectangle br = rect_cut_bottom(&in, 34);
        if (ui_button((Rectangle){ br.x + br.width - 92, br.y, 92, 34 }, "Close", 0, BTN_PRIMARY, true) || esc || enter)
            app.modal = MODAL_NONE;
        break;
    }
    }
    ui.layer = 0;
}

/* ---- keyboard ------------------------------------------------------------------------------------ */

static void global_keys(void)
{
    if (ui.no_input) return;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (app.modal) return;
    if (ctrl) {
        static const int order[] = { PAGE_PROCESSES, PAGE_PERFORMANCE, PAGE_ANALYSIS, PAGE_NETWORK, PAGE_DISK,
                                     PAGE_MEMORY, PAGE_STARTUP, PAGE_USERS, PAGE_SERVICES };
        for (int i = 0; i < 9; ++i)
            if (IsKeyPressed(KEY_ONE + i)) { app.page = order[i]; settings_save(); }
        if (IsKeyPressed(KEY_N)) open_run_dialog();
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) { app.zoom = fminf(2.0f, app.zoom + 0.1f); settings_save(); }
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) { app.zoom = fmaxf(0.8f, app.zoom - 0.1f); settings_save(); }
        if (IsKeyPressed(KEY_ZERO)) { app.zoom = 1.0f; settings_save(); }
    }
    if (IsKeyPressed(KEY_F1)) app.modal = MODAL_ABOUT;
    if (ui.focus == 0 && IsKeyPressed(KEY_SPACE)) { app.paused = !app.paused; sys_set_paused(app.paused); }
    if (IsKeyPressed(KEY_ESCAPE) && ui.focus == 0 && TL.pin >= 0) TL.pin = -1;
}

/* ---- frame --------------------------------------------------------------------------------------- */

static int want_for_page(int page)
{
    switch (page) {
    case PAGE_NETWORK:  return SYSWANT_CONNS;
    case PAGE_SERVICES: return SYSWANT_SERVICES;
    case PAGE_STARTUP:  return SYSWANT_STARTUP;
    case PAGE_USERS:    return SYSWANT_SESSIONS;
    default:            return 0;
    }
}

static void draw_frame(void)
{
    SysState* st = app.st;
    TL.count = st->samples;
    TL.latest = st->samples ? st->samples - 1 : 0;
    TL.window = WINDOWS_SEC[app.window_idx];
    TL.times = st->sample_time;
    if (TL.pin >= 0 && (uint64_t)TL.pin < tl_left()) TL.pin = -1;   /* scrolled out of the window */
    icons_upload();
    sys_want(want_for_page(app.page));
    global_keys();

    ClearBackground(T.bg);
    float side_w = app.nav_collapsed ? 64 : 232;
    Rectangle full = { 0, 0, ui.w, ui.h };
    draw_titlebar(rect_cut_top(&full, TITLE_H));
    Rectangle status = rect_cut_bottom(&full, 32);
    Rectangle side = rect_cut_left(&full, side_w);
    draw_sidebar(side);
    draw_statusbar(status);

    /* Firmware is read when its page is opened, not polled */
    static int last_page = -1;
    if (app.page != last_page && app.page == PAGE_FIRMWARE) app.fw_loaded = false;
    last_page = app.page;

    Rectangle page = rect_inset(full, 22, 0);
    page.height -= 16;
    switch (app.page) {
    case PAGE_PROCESSES:  page_processes(page); break;
    case PAGE_PERFORMANCE:page_performance(page); break;
    case PAGE_ANALYSIS:   page_analysis(page); break;
    case PAGE_NETWORK:    page_network(page); break;
    case PAGE_DISK:       page_disk(page); break;
    case PAGE_MEMORY:     page_memory(page); break;
    case PAGE_STARTUP:    page_startup(page); break;
    case PAGE_USERS:      page_users(page); break;
    case PAGE_SERVICES:   page_services(page); break;
    case PAGE_SETTINGS:   page_settings(page); break;
    case PAGE_FIRMWARE:   page_firmware(page); break;
    case PAGE_EVENTS:     page_events(page); break;
    }

    menubar_frame();

    int id = menu_run(&app.menu);
    if (id >= 0) menu_dispatch(id);
    draw_modal();
    toasts_draw();
    ui_frame_end();
}

static void usage(void)
{
    printf("Tarman %s - task manager and resource monitor for Windows\n\n"
           "Usage: tarman [options]\n\n"
           "Options:\n"
           "  -h, --help              Show this help and exit\n"
           "  -v, --version           Print the version and exit\n"
           "  --page NAME             Open on a page: processes, performance, analysis, network,\n"
           "                          disk, memory, startup, users, services, events, firmware, settings\n"
           "  --light | --dark        Force the theme for this run\n"
           "  --size WxH              Window size in logical pixels for this run\n"
           "  --window SECONDS        Visible time window: 60, 300 or 600\n"
           "  --log CHANNEL           Events page source, e.g. System (default: Tarman activity)\n"
           "  --demo                  Show a synthetic machine (for screenshots); actions are disabled\n"
           "  --screenshot FILE       Render, save a PNG of the window after --wait seconds, exit\n"
           "  --wait SECONDS          Delay before --screenshot (default 4)\n",
           TARMAN_VERSION);
}

int main(int argc, char** argv)
{
    const char* shot = NULL;
    double wait = 4.0;
    int force_theme = -1, force_page = -1, force_w = 0, force_h = 0, force_window = -1;
    const char* force_log = NULL;
    static const char* page_names[PAGE_COUNT] = { "processes", "performance", "analysis", "network", "disk",
                                                  "memory", "startup", "users", "services", "settings", "firmware", "events" };
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { sys_attach_console(); usage(); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) { sys_attach_console(); printf("tarman v%s\n", TARMAN_VERSION); return 0; }
        if (!strcmp(a, "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(a, "--wait") && i + 1 < argc) wait = atof(argv[++i]);
        else if (!strcmp(a, "--light")) force_theme = 0;
        else if (!strcmp(a, "--demo")) sys_set_demo(true);
        else if (!strcmp(a, "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &force_w, &force_h);
        else if (!strcmp(a, "--window") && i + 1 < argc) {
            int sec = atoi(argv[++i]);
            force_window = sec <= 60 ? 0 : sec <= 300 ? 1 : 2;
        }
        else if (!strcmp(a, "--log") && i + 1 < argc) force_log = argv[++i];
        else if (!strcmp(a, "--dark")) force_theme = 1;
        else if (!strcmp(a, "--page") && i + 1 < argc) {
            const char* p = argv[++i];
            for (int k = 0; k < PAGE_COUNT; ++k) if (!strcmp(p, page_names[k])) force_page = k;
        } else {
            sys_attach_console();
            fprintf(stderr, "tarman: unknown option '%s'\n\n", a);
            usage();
            return 2;
        }
    }

    app.dark = true;
    app.zoom = 1.0f;
    app.window_idx = 2;
    app.proc_sort = 0;
    app.an_sort = 2;
    app.an_desc = true;
    app.an_show_exited = true;
    app.disk_sort = 4;
    app.disk_desc = true;
    app.mem_sort = 6;
    app.mem_desc = true;
    app.st_sel = -1;
    app.hoswl = true;
    app.ev_time = 1;
    app.ev_sel = -1;
    app.ev_show_proc = true;
    TL.pin = -1;
    TL.hover = TL.hover_next = -1;
    settings_load();
    if (force_theme >= 0) app.dark = force_theme == 1;
    if (force_page >= 0) app.page = force_page;
    if (force_window >= 0) app.window_idx = force_window;
    if (force_w >= 900 && force_h >= 600) { g_win_w = force_w; g_win_h = force_h; }
    if (force_log) snprintf(app.ev_source, sizeof app.ev_source, "%s", force_log);
    ui_theme(app.dark);

    app.st = sys_start();
    if (!shot) menubar_init(app.hoswl);   /* screenshots must not take menubar clicks */

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIDDEN |
                   (shot ? FLAG_WINDOW_UNFOCUSED : 0));
    ui.no_input = shot != NULL;
    InitWindow(g_win_w, g_win_h, "Tarman");
    SetExitKey(KEY_NULL);
    float dpi = GetWindowScaleDPI().x;
    if (dpi < 1) dpi = 1;
    ui.scale = 0;
    ui_set_scale(dpi * app.zoom);
    int mon = GetCurrentMonitor();
    int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
    int ww = (int)(g_win_w * dpi), wh = (int)(g_win_h * dpi);
    if (ww > mw - 40) ww = mw - 40;
    if (wh > mh - 80) wh = mh - 80;
    SetWindowSize(ww, wh);
    SetWindowPosition((mw - ww) / 2, (mh - wh) / 2);
    SetWindowMinSize((int)(900 * dpi), (int)(600 * dpi));
    Image icons[3] = { logo_image(32), logo_image(64), logo_image(256) };
    SetWindowIcons(icons, 3);
    for (int i = 0; i < 3; ++i) UnloadImage(icons[i]);
    sys_window_setup(GetWindowHandle(), app.dark, app.topmost);
    sys_titlebar_install(GetWindowHandle());
    ClearWindowState(FLAG_WINDOW_HIDDEN);
    ui_fonts_init();

    bool last_dark = app.dark, last_top = app.topmost;
    double last_input = GetTime(), start = GetTime();
    while (!WindowShouldClose() && !app.quit) {
        /* a monitor should not be the thing burning CPU: full rate only
         * while the user is interacting, a trickle otherwise */
        Vector2 md = GetMouseDelta();
        if (md.x || md.y || GetKeyPressed() || GetMouseWheelMove() || IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            last_input = GetTime();
        int fps = GetTime() - last_input < 2.0 ? 60 : (IsWindowFocused() ? 12 : 6);
        if (IsWindowMinimized()) fps = 2;
        if (shot) fps = 30;
        SetTargetFPS(fps);

        float scale = GetWindowScaleDPI().x;
        if (scale < 1) scale = 1;
        ui_set_scale(scale * app.zoom);
        if (app.dark != last_dark || app.topmost != last_top) {
            sys_window_setup(GetWindowHandle(), app.dark, app.topmost);
            last_dark = app.dark;
            last_top = app.topmost;
        }

        ui_frame_begin();
        BeginDrawing();
        Camera2D cam = { .zoom = ui.scale };
        BeginMode2D(cam);
        sys_lock();
        draw_frame();
        sys_unlock();
        EndMode2D();

        if (shot && GetTime() - start > wait) {
            rlDrawRenderBatchActive();
            Image img = LoadImageFromScreen();
            ExportImage(img, shot);
            UnloadImage(img);
            app.quit = true;
        }
        EndDrawing();
    }

    if (!shot) settings_save();
    menubar_shutdown();
    sys_stop();
    icons_free();
    ui_fonts_free();
    CloseWindow();
    return 0;
}
