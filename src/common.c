/* Helpers shared by the pages: toasts, process lookup and icons, the process
 * context menu and its actions, sortable tables, headers and stat blocks. */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

const PrioClass PRIO_CLASSES[6] = {
    { "Realtime",     0x00000100, 24 },
    { "High",         0x00000080, 13 },
    { "Above normal", 0x00008000, 10 },
    { "Normal",       0x00000020, 8 },
    { "Below normal", 0x00004000, 6 },
    { "Low",          0x00000040, 4 },
};

/* ---- toasts ------------------------------------------------------------------- */

void toast(bool error, const char* fmt, ...)
{
    if (app.ntoasts == 4) {
        memmove(&app.toasts[0], &app.toasts[1], sizeof app.toasts[0] * 3);
        app.ntoasts = 3;
    }
    Toast* t = &app.toasts[app.ntoasts++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->text, sizeof t->text, fmt, ap);
    va_end(ap);
    t->error = error;
    t->until = GetTime() + (error ? 6.0 : 4.0);
    sys_log(error ? LOG_LV_ERROR : LOG_LV_INFO, "Action", 0, true, "%s", t->text);
}

void toasts_draw(void)
{
    double now = GetTime();
    int w = 0;
    for (int i = 0; i < app.ntoasts; ++i) if (app.toasts[i].until > now) app.toasts[w++] = app.toasts[i];
    app.ntoasts = w;
    float y = ui.h - 44;
    for (int i = app.ntoasts - 1; i >= 0; --i) {
        Toast* t = &app.toasts[i];
        float tw = fminf(ui.w - 40, ui_text_w(t->text, 13, FW_REG) + 56);
        Rectangle r = { ui.w - tw - 20, y - 44, tw, 40 };
        float a = (float)fmin(1.0, (t->until - now) / 0.4);
        ui_rrect((Rectangle){ r.x, r.y + 3, r.width, r.height }, 8, col_alpha(BLACK, 0.35f * a));
        ui_rrect(r, 8, col_alpha(T.dark ? (Color){ 0x1E, 0x25, 0x31, 0xFF } : T.panel, a));
        Color c = t->error ? T.bad : T.good;
        ui_rrect_line(r, 8, 1, col_alpha(c, 0.5f * a));
        ui_icon(r.x + 20, r.y + 20, t->error ? IC_WARNING : IC_CHECK, 14, col_alpha(c, a));
        ui_text_fit((Rectangle){ r.x + 38, r.y, r.width - 48, r.height }, t->text, 13, FW_REG,
                    col_alpha(T.text, a), AL_LEFT);
        ui_overlay(r);
        y -= 50;
    }
}

/* ---- processes ------------------------------------------------------------------ */

Proc* proc_find(uint32_t pid, uint64_t create)
{
    SysState* st = app.st;
    for (int i = 0; i < st->nproc; ++i)
        if (st->procs[i]->pid == pid && st->procs[i]->create_time == create) return st->procs[i];
    return NULL;
}

Proc* proc_find_alive(uint32_t pid)
{
    SysState* st = app.st;
    for (int i = 0; i < st->nproc; ++i)
        if (st->procs[i]->pid == pid && st->procs[i]->alive) return st->procs[i];
    return NULL;
}

Proc* proc_selected(void)
{
    if (!app.sel_pid && !app.sel_create) return NULL;
    return proc_find(app.sel_pid, app.sel_create);
}

void proc_select(const Proc* p)
{
    app.sel_pid = p ? p->pid : 0;
    app.sel_create = p ? p->create_time : 0;
}

const char* proc_display_name(const Proc* p)
{
    return p->desc[0] ? p->desc : p->name;
}

const char* prio_name(int32_t b)
{
    if (b >= 16) return "Realtime";
    if (b >= 13) return "High";
    if (b >= 9)  return "Above normal";
    if (b >= 7)  return "Normal";
    if (b >= 5)  return "Below normal";
    return "Low";
}

bool text_match(const char* hay, const char* needle)
{
    if (!needle[0]) return true;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

void icons_upload(void)
{
    SysState* st = app.st;
    if (st->nicon > app.icon_tex_cap) {
        app.icon_tex_cap = st->nicon + 64;
        app.icon_tex = realloc(app.icon_tex, sizeof *app.icon_tex * app.icon_tex_cap);
    }
    for (; app.nicon_tex < st->nicon; ++app.nicon_tex) {
        IconEntry* e = &st->icons[app.nicon_tex];
        Image img = { e->rgba, e->w, e->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        Texture2D t = LoadTextureFromImage(img);
        GenTextureMipmaps(&t);
        SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
        app.icon_tex[app.nicon_tex] = t;
    }
}

void icons_free(void)
{
    for (int i = 0; i < app.nicon_tex; ++i) UnloadTexture(app.icon_tex[i]);
    free(app.icon_tex);
    app.icon_tex = NULL;
    app.nicon_tex = app.icon_tex_cap = 0;
}

void proc_icon(const Proc* p, float cx, float cy, float size)
{
    if (p->icon >= 0 && p->icon < app.nicon_tex) {
        Texture2D t = app.icon_tex[p->icon];
        DrawTexturePro(t, (Rectangle){ 0, 0, (float)t.width, (float)t.height },
                       (Rectangle){ cx - size / 2, cy - size / 2, size, size }, (Vector2){ 0 }, 0, WHITE);
        return;
    }
    /* no icon: a tinted tile with the initial, hue derived from the name */
    uint32_t h = 0;
    for (const char* s = p->name; *s; ++s) h = h * 31 + (uint8_t)*s;
    Color c = ColorFromHSV((float)(h % 360), T.dark ? 0.45f : 0.55f, T.dark ? 0.75f : 0.8f);
    Rectangle r = { cx - size / 2, cy - size / 2, size, size };
    ui_rrect(r, size * 0.25f, col_alpha(c, 0.22f));
    char ch[2] = { p->name[0] ? p->name[0] : '?', 0 };
    if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] -= 32;
    ui_text_fit(r, ch, size * 0.55f, FW_BOLD, c, AL_CENTER);
}

/* ---- the process menu ---------------------------------------------------------------- */

void proc_menu_open(const Proc* p, Vector2 at)
{
    Menu* m = &app.menu;
    menu_open(m, at, MENU_OWNER_PROC);
    m->key_a = p->pid;
    m->key_b = p->create_time;
    ProcExtra ex;
    sys_proc_extra(p->pid, &ex);
    bool alive = p->alive;
    menu_add(m, "End task", IC_CANCEL, PA_END, alive, false);
    menu_add(m, "End process tree", IC_STOP, PA_END_TREE, alive, false);
    menu_sep(m);
    if (p->is_suspended) menu_add(m, "Resume", IC_PLAY, PA_RESUME, alive, false);
    else menu_add(m, "Suspend", IC_PAUSE, PA_SUSPEND, alive, false);
    menu_add(m, "Efficiency mode", IC_LEAF, PA_EFFICIENCY, alive && ex.ok, ex.efficiency);
    int pr = menu_add_sub(m, "Set priority", IC_SORT);
    for (int i = 0; i < 6; ++i)
        menu_add_child(m, pr, PRIO_CLASSES[i].name, PA_PRIO_BASE + i, alive, ex.prio_class == PRIO_CLASSES[i].cls);
    menu_add(m, "Set affinity\xE2\x80\xA6", IC_CHIP, PA_AFFINITY, alive && ex.ok, false);
    menu_sep(m);
    menu_add(m, "Create memory dump file", IC_DOWNLOAD, PA_DUMP, alive, false);
    menu_add(m, "Open file location", IC_FOLDER, PA_LOCATION, p->path[0] != 0, false);
    menu_add(m, "Search online", IC_SEARCH, PA_SEARCH, true, false);
    menu_add(m, "Properties", IC_INFO, PA_PROPERTIES, p->path[0] != 0, false);
    menu_sep(m);
    int cp = menu_add_sub(m, "Copy", IC_COPY);
    menu_add_child(m, cp, "Name", PA_COPY_NAME, true, false);
    menu_add_child(m, cp, "PID", PA_COPY_PID, true, false);
    menu_add_child(m, cp, "Path", PA_COPY_PATH, p->path[0] != 0, false);
    menu_add_child(m, cp, "Command line", PA_COPY_CMD, p->cmdline[0] != 0, false);
    if (app.page != PAGE_PROCESSES) {
        menu_sep(m);
        menu_add(m, "Go to process", IC_PROCESSES, PA_GOTO_PROCESS, alive, false);
    }
}

void open_confirm(int action, const Proc* p)
{
    app.modal = MODAL_CONFIRM;
    app.modal_action = action;
    app.modal_pid = p->pid;
    app.modal_create = p->create_time;
    snprintf(app.modal_name, sizeof app.modal_name, "%s", p->name);
    if (action == PA_END_TREE) {
        snprintf(app.modal_title, sizeof app.modal_title, "End process tree?");
        snprintf(app.modal_text, sizeof app.modal_text,
                 "%s (PID %u) and every process it started will be terminated. Unsaved data will be lost.",
                 p->name, p->pid);
    } else {
        snprintf(app.modal_title, sizeof app.modal_title, "End %s?", p->name);
        snprintf(app.modal_text, sizeof app.modal_text,
                 "%s (PID %u) is a Windows process. Ending it may make Windows unstable or shut it down.",
                 p->name, p->pid);
    }
}

void proc_action(int action, uint32_t pid, uint64_t create)
{
    Proc* p = proc_find(pid, create);
    if (!p) { toast(true, "That process has already exited."); return; }
    char err[256] = "";
    switch (action) {
    case PA_END:
        if (p->group == PGROUP_WINDOWS && app.modal != MODAL_CONFIRM) { open_confirm(PA_END, p); return; }
        if (sys_kill(pid, err, sizeof err)) toast(false, "Ended %s (%u)", p->name, pid);
        else toast(true, "Couldn't end %s: %s", p->name, err);
        break;
    case PA_END_TREE:
        if (app.modal != MODAL_CONFIRM) { open_confirm(PA_END_TREE, p); return; }
        if (sys_kill_tree(pid, err, sizeof err)) toast(false, "Ended %s and its children", p->name);
        else toast(true, "Couldn't end %s: %s", p->name, err);
        break;
    case PA_SUSPEND:
    case PA_RESUME:
        if (sys_suspend(pid, action == PA_RESUME, err, sizeof err))
            toast(false, "%s %s", action == PA_RESUME ? "Resumed" : "Suspended", p->name);
        else toast(true, "Couldn't %s %s: %s", action == PA_RESUME ? "resume" : "suspend", p->name, err);
        break;
    case PA_EFFICIENCY: {
        ProcExtra ex;
        sys_proc_extra(pid, &ex);
        if (sys_set_efficiency(pid, !ex.efficiency, err, sizeof err))
            toast(false, "Efficiency mode %s for %s", ex.efficiency ? "off" : "on", p->name);
        else toast(true, "Couldn't change efficiency mode: %s", err);
        app.extra_time = 0;
        break;
    }
    case PA_AFFINITY:
        open_affinity_dialog(p);
        break;
    case PA_DUMP: {
        char path[300];
        if (sys_create_dump(pid, p->name, path, sizeof path, err, sizeof err)) toast(false, "Dump written to %s", path);
        else toast(true, "Couldn't create dump: %s", err);
        break;
    }
    case PA_LOCATION:   sys_open_location(p->path); break;
    case PA_SEARCH:     sys_search_online(p->name); break;
    case PA_PROPERTIES: sys_open_properties(p->path); break;
    case PA_COPY_NAME:  SetClipboardText(p->name); toast(false, "Copied name"); break;
    case PA_COPY_PID: {
        char b[16];
        snprintf(b, sizeof b, "%u", p->pid);
        SetClipboardText(b);
        toast(false, "Copied PID %s", b);
        break;
    }
    case PA_COPY_PATH:  SetClipboardText(p->path); toast(false, "Copied path"); break;
    case PA_COPY_CMD:   SetClipboardText(p->cmdline); toast(false, "Copied command line"); break;
    case PA_DETAILS:
        proc_select(p);
        app.details_open = true;
        break;
    case PA_GOTO_PROCESS:
        proc_select(p);
        app.page = PAGE_PROCESSES;
        app.search[0] = 0;
        app.details_open = true;
        app.scroll_to_sel = true;
        break;
    default:
        if (action >= PA_PRIO_BASE && action < PA_PRIO_BASE + 6) {
            const PrioClass* pc = &PRIO_CLASSES[action - PA_PRIO_BASE];
            if (sys_set_priority(pid, pc->cls, err, sizeof err)) toast(false, "%s priority set to %s", p->name, pc->name);
            else toast(true, "Couldn't set priority: %s", err);
            app.extra_time = 0;
        }
        break;
    }
}

/* page-specific owners are dispatched in their own files */
void services_menu_action(int id);
void startup_menu_action(int id);
void users_menu_action(int id);

void menu_dispatch(int id)
{
    switch (app.menu.owner) {
    case MENU_OWNER_PROC:    proc_action(id, app.menu.key_a, app.menu.key_b); break;
    case MENU_OWNER_SERVICE: services_menu_action(id); break;
    case MENU_OWNER_STARTUP: startup_menu_action(id); break;
    case MENU_OWNER_SESSION: users_menu_action(id); break;
    default: break;
    }
}

/* ---- tables ------------------------------------------------------------------------- */

void table_layout(const Column* cols, int n, Rectangle r, ColLayout* L, float flex_min)
{
    L->n = n;
    for (int i = 0; i < n; ++i) L->vis[i] = true;
    for (;;) {
        float fixed = 0;
        int nflex = 0, worst = -1;
        for (int i = 0; i < n; ++i) {
            if (!L->vis[i]) continue;
            if (cols[i].w > 0) fixed += cols[i].w; else nflex++;
            if (cols[i].prio > 0 && (worst < 0 || cols[i].prio > cols[worst].prio)) worst = i;
        }
        if (fixed + nflex * flex_min <= r.width || worst < 0) {
            float flex = nflex ? fmaxf(flex_min, (r.width - fixed) / nflex) : 0;
            float x = r.x;
            for (int i = 0; i < n; ++i) {
                if (!L->vis[i]) { L->x[i] = x; L->w[i] = 0; continue; }
                L->x[i] = x;
                L->w[i] = cols[i].w > 0 ? cols[i].w : flex;
                x += L->w[i];
            }
            return;
        }
        L->vis[worst] = false;
    }
}

Rectangle table_cell_rect(Rectangle row, const ColLayout* L, int c)
{
    return (Rectangle){ L->x[c] + 10, row.y, L->w[c] - 20, row.height };
}

void table_cell(Rectangle row, const ColLayout* L, int c, const char* s, Color col, int weight, int align)
{
    if (!L->vis[c]) return;
    ui_text_fit(table_cell_rect(row, L, c), s, weight == FW_MONO ? 11.5f : 13, weight, col, align);
}

bool table_header(Rectangle r, const Column* cols, const ColLayout* L, int* sort, bool* desc)
{
    bool changed = false;
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 1, r.width, 1 }, T.border);
    for (int i = 0; i < L->n; ++i) {
        if (!L->vis[i]) continue;
        Rectangle c = { L->x[i], r.y, L->w[i], r.height };
        bool hov = ui_hover(c);
        if (hov) { ui_rrect(rect_inset(c, 2, 3), 5, T.hover); ui.cursor = MOUSE_CURSOR_POINTING_HAND; }
        bool on = *sort == i;
        Rectangle tr = table_cell_rect(c, L, i);
        tr.x = c.x + 10; tr.width = c.width - 20;
        if (on) {
            if (cols[i].align == AL_RIGHT) { tr.width -= 14; ui_icon(c.x + c.width - 12, r.y + r.height / 2, *desc ? IC_CHEV_D : IC_CHEV_U, 8, T.accent); }
            else { float tw = ui_text_w(cols[i].name, 12, FW_SEMI); ui_icon(fminf(tr.x + tw + 10, c.x + c.width - 10), r.y + r.height / 2, *desc ? IC_CHEV_D : IC_CHEV_U, 8, T.accent); }
        }
        ui_text_fit(tr, cols[i].name, 12, FW_SEMI, on ? T.text : T.dim, cols[i].align);
        if (ui_clicked(c)) {
            if (*sort == i) *desc = !*desc;
            else { *sort = i; *desc = cols[i].align == AL_RIGHT; }
            changed = true;
        }
        if (i > 0) DrawRectangleRec((Rectangle){ c.x, r.y + 9, 1, r.height - 18 }, col_alpha(T.border, 0.7f));
    }
    return changed;
}

int table_scroll(Rectangle body, float* scroll, int rows, float row_h, uint32_t id)
{
    float content = rows * row_h;
    ui_wheel(body, scroll, row_h * 3);
    float maxs = fmaxf(0, content - body.height);
    if (*scroll > maxs) *scroll = maxs;
    if (*scroll < 0) *scroll = 0;
    ui_scrollbar((Rectangle){ body.x + body.width - 10, body.y + 2, 10, body.height - 4 }, scroll, content, body.height, id);
    return (int)(*scroll / row_h);
}

/* ---- layout bits -------------------------------------------------------------------- */

Rectangle page_header(Rectangle* r, const char* title, const char* subtitle)
{
    Rectangle h = rect_cut_top(r, 58);
    float tw = ui_text_w(title, 20, FW_SEMI);
    ui_text_fit((Rectangle){ h.x, h.y + 10, tw + 4, 30 }, title, 20, FW_SEMI, T.text, AL_LEFT);
    float used = tw + 16;
    if (subtitle && subtitle[0]) {
        float sw = fminf(ui_text_w(subtitle, 12.5f, FW_REG) + 4, fmaxf(0, h.width * 0.35f));
        ui_text_fit((Rectangle){ h.x + used, h.y + 14, sw, 26 }, subtitle, 12.5f, FW_REG, T.faint, AL_LEFT);
        used += sw + 12;
    }
    return (Rectangle){ h.x + used, h.y + 10, fmaxf(0, h.width - used), 32 };
}

void card(Rectangle r)
{
    ui_rrect(r, 10, T.panel);
    ui_rrect_line(r, 10, 1, T.border);
}

void stat_item(float x, float y, float w, const char* label, const char* value, Color accent)
{
    ui_text_fit((Rectangle){ x, y, w, 16 }, label, 11.5f, FW_REG, T.dim, AL_LEFT);
    if (accent.a) DrawRectangleRec((Rectangle){ x - 8, y + 3, 2.5f, 34 }, accent);
    ui_text_fit((Rectangle){ x, y + 16, w, 24 }, value, 16.5f, FW_SEMI, T.text, AL_LEFT);
}

void empty_state(Rectangle r, int icon, const char* text)
{
    ui_icon(r.x + r.width / 2, r.y + r.height / 2 - 18, icon, 28, T.faint);
    ui_text_fit((Rectangle){ r.x, r.y + r.height / 2 + 6, r.width, 22 }, text, 13, FW_REG, T.faint, AL_CENTER);
}

void legend_dot(float x, float cy, Color c, const char* label)
{
    DrawCircleV((Vector2){ x + 4, cy }, 4, c);
    ui_text_fit((Rectangle){ x + 13, cy - 9, ui_text_w(label, 12, FW_REG) + 4, 18 }, label, 12, FW_REG, T.dim, AL_LEFT);
}

WinStats win_stats(const float* ring, uint64_t first, uint64_t last)
{
    WinStats s = { 0 };
    uint64_t a = first > tl_left() ? first : tl_left();
    uint64_t b = last < TL.latest ? last : TL.latest;
    if (!TL.count || a > b) return s;
    static float tmp[HIST_LEN];
    int n = 0;
    double sum = 0;
    for (uint64_t k = a; k <= b && n < HIST_LEN; ++k) {
        float v = hist_at(ring, k);
        tmp[n++] = v;
        sum += v;
        if (v > s.peak) s.peak = v;
    }
    s.avg = n ? (float)(sum / n) : 0;
    s.last = hist_at(ring, b);
    /* p95 via partial selection (n <= 600, a simple sort is fine) */
    for (int i = 1; i < n; ++i) {
        float v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = v;
    }
    s.p95 = n ? tmp[(int)floorf((n - 1) * 0.95f)] : 0;
    return s;
}
