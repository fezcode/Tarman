/* Processes page: Task Manager's Processes + Details tabs in one table
 * (grouped like Task Manager, as a parent/child tree like Process Explorer,
 * or flat), with a details drawer that carries the selected process's own
 * ten-minute history. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

enum { C_NAME, C_STATUS, C_PID, C_USER, C_CPU, C_MEM, C_DISK, C_GPU, C_THREADS, C_HANDLES, C_TREND, C_COUNT };

static const Column COLS[C_COUNT] = {
    { "Name",     0,   AL_LEFT,  0 },
    { "Status",   92,  AL_LEFT,  5 },
    { "PID",      68,  AL_RIGHT, 3 },
    { "User",     112, AL_LEFT,  6 },
    { "CPU",      76,  AL_RIGHT, 0 },
    { "Memory",   96,  AL_RIGHT, 0 },
    { "Disk",     90,  AL_RIGHT, 2 },
    { "GPU",      68,  AL_RIGHT, 4 },
    { "Threads",  70,  AL_RIGHT, 8 },
    { "Handles",  76,  AL_RIGHT, 9 },
    { "CPU \xC2\xB7 window", 124, AL_LEFT, 7 },
};

typedef struct {
    Proc* p;
    int   group;          /* header rows: group id; proc rows: -1 */
    int   depth;
    bool  has_kids;
    int   count;
} Row;

#define MAX_ROWS 8192
static Row   g_rows[MAX_ROWS];
static int   g_nrows;
static Proc* g_list[MAX_ROWS];

static int  s_sort;
static bool s_desc;

static int cmp_str(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return (unsigned char)x - (unsigned char)y;
    }
}

#define CMPNUM(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)

static int proc_cmp(const void* A, const void* B)
{
    const Proc* a = *(Proc* const*)A;
    const Proc* b = *(Proc* const*)B;
    int r = 0;
    switch (s_sort) {
    case C_NAME:    r = cmp_str(proc_display_name(a), proc_display_name(b)); break;
    case C_STATUS:  r = CMPNUM(a->is_suspended, b->is_suspended); break;
    case C_PID:     r = CMPNUM(a->pid, b->pid); break;
    case C_USER:    r = cmp_str(a->user, b->user); break;
    case C_CPU:     r = CMPNUM(a->cpu, b->cpu); break;
    case C_MEM:     r = CMPNUM(a->ws_private, b->ws_private); break;
    case C_DISK:    r = CMPNUM(a->io_read_bps + a->io_write_bps, b->io_read_bps + b->io_write_bps); break;
    case C_GPU:     r = CMPNUM(a->gpu, b->gpu); break;
    case C_THREADS: r = CMPNUM(a->threads, b->threads); break;
    case C_HANDLES: r = CMPNUM(a->handles, b->handles); break;
    case C_TREND:   r = CMPNUM(a->cpu, b->cpu); break;
    }
    if (s_desc) r = -r;
    if (!r) r = CMPNUM(a->pid, b->pid);
    return r;
}

static bool proc_visible(const Proc* p)
{
    if (!p->alive) return false;
    if (!app.search[0]) return true;
    char pid[16];
    snprintf(pid, sizeof pid, "%u", p->pid);
    return text_match(p->name, app.search) || text_match(p->desc, app.search) ||
           text_match(p->title, app.search) || text_match(p->user, app.search) ||
           !strcmp(pid, app.search);
}

static bool tree_is_collapsed(uint32_t pid)
{
    for (int i = 0; i < app.ntree_collapsed; ++i) if (app.tree_collapsed[i] == pid) return true;
    return false;
}

static void tree_toggle(uint32_t pid)
{
    for (int i = 0; i < app.ntree_collapsed; ++i)
        if (app.tree_collapsed[i] == pid) { app.tree_collapsed[i] = app.tree_collapsed[--app.ntree_collapsed]; return; }
    if (app.ntree_collapsed < 256) app.tree_collapsed[app.ntree_collapsed++] = pid;
}

static Proc* parent_of(Proc* p, Proc** list, int n)
{
    for (int i = 0; i < n; ++i) {
        Proc* q = list[i];
        if (q->pid == p->ppid && q != p && q->create_time <= p->create_time) return q;
    }
    return NULL;
}

static void tree_add(Proc* p, Proc** list, Proc** parents, int n, int depth)
{
    if (g_nrows >= MAX_ROWS || depth > 48) return;
    Row* r = &g_rows[g_nrows++];
    *r = (Row){ p, -1, depth, false, 0 };
    int first_child = -1;
    for (int i = 0; i < n; ++i) if (parents[i] == p) { first_child = i; break; }
    r->has_kids = first_child >= 0;
    if (!r->has_kids || tree_is_collapsed(p->pid)) return;
    for (int i = 0; i < n; ++i) if (parents[i] == p) tree_add(list[i], list, parents, n, depth + 1);
}

static void build_rows(void)
{
    SysState* st = app.st;
    int n = 0;
    for (int i = 0; i < st->nproc && n < MAX_ROWS; ++i)
        if (proc_visible(st->procs[i])) g_list[n++] = st->procs[i];
    s_sort = app.proc_sort;
    s_desc = app.proc_desc;
    qsort(g_list, (size_t)n, sizeof g_list[0], proc_cmp);
    g_nrows = 0;

    int view = app.proc_view;
    if (view == 1 && app.search[0]) view = 2;   /* a filtered tree is mostly orphans */
    if (view == 0) {
        static const char* names[PGROUP_COUNT] = { "Apps", "Background processes", "Windows processes" };
        (void)names;
        for (int g = 0; g < PGROUP_COUNT; ++g) {
            int cnt = 0;
            for (int i = 0; i < n; ++i) if (g_list[i]->group == g) cnt++;
            if (!cnt) continue;
            g_rows[g_nrows++] = (Row){ NULL, g, 0, false, cnt };
            if (app.group_collapsed[g]) continue;
            for (int i = 0; i < n && g_nrows < MAX_ROWS; ++i)
                if (g_list[i]->group == g) g_rows[g_nrows++] = (Row){ g_list[i], -1, 0, false, 0 };
        }
    } else if (view == 1) {
        static Proc* parents[MAX_ROWS];
        for (int i = 0; i < n; ++i) parents[i] = parent_of(g_list[i], g_list, n);
        for (int i = 0; i < n; ++i) if (!parents[i]) tree_add(g_list[i], g_list, parents, n, 0);
    } else {
        for (int i = 0; i < n; ++i) g_rows[g_nrows++] = (Row){ g_list[i], -1, 0, false, 0 };
    }
}

/* ---- the table ------------------------------------------------------------------ */

static void metric_cell(Rectangle row, const ColLayout* L, int c, const char* text, float heat)
{
    if (!L->vis[c]) return;
    Rectangle cell = { L->x[c] + 1, row.y + 1, L->w[c] - 2, row.height - 2 };
    Color h = ui_heat(heat);
    if (h.a) DrawRectangleRec(cell, h);
    table_cell(row, L, c, text, heat > 0.5f ? T.text : (heat > 0.01f ? T.text : T.dim), FW_REG, AL_RIGHT);
}

static void header_totals(Rectangle r, const ColLayout* L)
{
    SysState* st = app.st;
    double io = 0;
    float gpu = 0;
    for (int i = 0; i < st->nproc; ++i) {
        Proc* p = st->procs[i];
        if (!p->alive) continue;
        io += p->io_read_bps + p->io_write_bps;
    }
    for (int i = 0; i < st->ngpu; ++i) gpu = fmaxf(gpu, st->gpu[i].util);
    char b[32];
    struct { int col; float v; const char* fmt; Color c; } tot[] = {
        { C_CPU, st->cpu.usage, "%.0f%%", T.cpu },
        { C_MEM, st->mem.load, "%.0f%%", T.mem },
        { C_GPU, gpu, "%.0f%%", T.gpu },
    };
    for (size_t i = 0; i < sizeof tot / sizeof tot[0]; ++i) {
        if (!L->vis[tot[i].col]) continue;
        snprintf(b, sizeof b, tot[i].fmt, tot[i].v);
        Rectangle c = table_cell_rect(r, L, tot[i].col);
        ui_text_fit(c, b, 17, FW_SEMI, tot[i].v > 80 ? T.warn : T.text, AL_RIGHT);
    }
    if (L->vis[C_DISK]) {
        fmt_rate(io, b, sizeof b);
        ui_text_fit(table_cell_rect(r, L, C_DISK), b, 14, FW_SEMI, T.text, AL_RIGHT);
    }
}

static void move_selection(int dir)
{
    int cur = -1;
    for (int i = 0; i < g_nrows; ++i)
        if (g_rows[i].p && g_rows[i].p->pid == app.sel_pid && g_rows[i].p->create_time == app.sel_create) cur = i;
    int i = cur;
    for (;;) {
        i += dir;
        if (i < 0 || i >= g_nrows) return;
        if (g_rows[i].p) break;
    }
    proc_select(g_rows[i].p);
    app.scroll_to_sel = true;
}

static void draw_table(Rectangle r)
{
    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    ColLayout L;
    table_layout(COLS, C_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 200);

    Rectangle head = rect_cut_top(&in, 58);
    header_totals((Rectangle){ head.x, head.y + 6, head.width, 26 }, &L);
    if (table_header((Rectangle){ head.x, head.y + 28, head.width, 30 }, COLS, &L, &app.proc_sort, &app.proc_desc))
        app.proc_scroll = 0;

    const float RH = 32;
    Rectangle body = rect_inset(in, 0, 2);
    if (app.scroll_to_sel) {
        for (int i = 0; i < g_nrows; ++i)
            if (g_rows[i].p && g_rows[i].p->pid == app.sel_pid && g_rows[i].p->create_time == app.sel_create) {
                float y = i * RH;
                if (y < app.proc_scroll) app.proc_scroll = y;
                if (y + RH > app.proc_scroll + body.height) app.proc_scroll = y + RH - body.height;
            }
        app.scroll_to_sel = false;
    }
    int first = table_scroll(body, &app.proc_scroll, g_nrows, RH, ui_id("proc-scroll", 0));
    ui_scissor_push(body);
    float total_mem = (float)(app.st->mem.total ? app.st->mem.total : 1);
    static const char* gnames[PGROUP_COUNT] = { "Apps", "Background processes", "Windows processes" };
    for (int i = first; i < g_nrows; ++i) {
        float y = body.y + i * RH - app.proc_scroll;
        if (y > body.y + body.height) break;
        Row* row = &g_rows[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);

        if (!row->p) {
            /* group header */
            if (hov) ui_rrect(rr, 6, T.hover);
            bool col = app.group_collapsed[row->group];
            ui_icon(rr.x + 14, rr.y + RH / 2, col ? IC_CHEV_R : IC_CHEV_D, 10, T.dim);
            char b[64];
            snprintf(b, sizeof b, "%s (%d)", gnames[row->group], row->count);
            ui_text_fit((Rectangle){ rr.x + 30, rr.y, 300, RH }, b, 13.5f, FW_SEMI, T.text, AL_LEFT);
            if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
            if (hov && ui_clicked(rr)) app.group_collapsed[row->group] = !col;
            continue;
        }

        Proc* p = row->p;
        bool sel = p->pid == app.sel_pid && p->create_time == app.sel_create;
        if (sel) {
            ui_rrect(rr, 6, T.sel);
            ui_rrect((Rectangle){ rr.x, rr.y + 8, 3, RH - 16 }, 1.5f, T.accent);
        } else if (hov) ui_rrect(rr, 6, T.hover);

        char b[64];
        float t;
        /* metrics first so their heat tint sits under the text */
        snprintf(b, sizeof b, "%.1f%%", p->cpu);
        t = p->cpu / 40.0f;
        metric_cell(rr, &L, C_CPU, b, t);
        fmt_bytes((double)p->ws_private, b, sizeof b);
        metric_cell(rr, &L, C_MEM, b, (float)p->ws_private / total_mem * 6);
        float io = p->io_read_bps + p->io_write_bps;
        fmt_rate(io, b, sizeof b);
        metric_cell(rr, &L, C_DISK, b, io / (32.0f * 1024 * 1024));
        snprintf(b, sizeof b, "%.1f%%", p->gpu);
        metric_cell(rr, &L, C_GPU, b, p->gpu / 50.0f);

        /* name: indent, chevron, icon, label */
        Rectangle nc = table_cell_rect(rr, &L, C_NAME);
        float x = nc.x + (app.proc_view == 0 ? 18 : 0) + row->depth * 18;
        if (row->has_kids) {
            Rectangle ch = { x - 2, rr.y + 6, 18, RH - 12 };
            bool ch_hov = ui_hover(ch);
            if (ch_hov) ui_rrect(ch, 4, T.press);
            ui_icon(ch.x + 9, rr.y + RH / 2, tree_is_collapsed(p->pid) ? IC_CHEV_R : IC_CHEV_D, 9, T.dim);
            if (ch_hov && ui_clicked(ch)) tree_toggle(p->pid);
        }
        if (app.proc_view == 1) x += 18;
        proc_icon(p, x + 9, rr.y + RH / 2, 18);
        x += 28;
        const char* label = proc_display_name(p);
        Rectangle lr = { x, rr.y, nc.x + nc.width - x, RH };
        if (p->is_app && p->title[0] && strcmp(p->title, label) != 0) {
            float lw = fminf(ui_text_w(label, 13, FW_REG) + 2, lr.width);
            ui_text_fit((Rectangle){ lr.x, lr.y, lw, RH }, label, 13, FW_REG, T.text, AL_LEFT);
            ui_text_fit((Rectangle){ lr.x + lw + 8, lr.y, lr.width - lw - 8, RH }, p->title, 12, FW_REG, T.faint, AL_LEFT);
        } else {
            ui_text_fit(lr, label, 13, FW_REG, T.text, AL_LEFT);
        }

        if (L.vis[C_STATUS] && p->is_suspended) {
            Rectangle sc = table_cell_rect(rr, &L, C_STATUS);
            ui_badge(sc.x, rr.y + RH / 2, "Suspended", T.warn);
        }
        snprintf(b, sizeof b, "%u", p->pid);
        table_cell(rr, &L, C_PID, b, T.dim, FW_REG, AL_RIGHT);
        table_cell(rr, &L, C_USER, p->user, T.dim, FW_REG, AL_LEFT);
        snprintf(b, sizeof b, "%u", p->threads);
        table_cell(rr, &L, C_THREADS, b, T.dim, FW_REG, AL_RIGHT);
        snprintf(b, sizeof b, "%u", p->handles);
        table_cell(rr, &L, C_HANDLES, b, T.dim, FW_REG, AL_RIGHT);
        if (L.vis[C_TREND]) {
            Rectangle sc = table_cell_rect(rr, &L, C_TREND);
            ui_sparkline((Rectangle){ sc.x, rr.y + 6, sc.width, RH - 12 }, p->h_cpu, p->first_sample, p->last_sample, 0, T.cpu);
        }

        if (hov) {
            if (ui_rclicked(rr)) { proc_select(p); proc_menu_open(p, ui.mouse); }
            else if (ui_dclicked(rr)) { proc_select(p); app.details_open = !app.details_open || !sel; }
            else if (ui_clicked(rr)) proc_select(p);
        }
    }
    ui_scissor_pop();
    if (!g_nrows) empty_state(body, IC_SEARCH, app.search[0] ? "No processes match your search" : "Collecting\xE2\x80\xA6");
}

/* ---- details drawer --------------------------------------------------------------------------- */

static float wrap_text(Rectangle r, const char* s, float size, Color c, bool draw)
{
    /* greedy wrap by measured prefix; breaks anywhere for paths and command lines */
    float lh = size * 1.45f, y = r.y;
    char line[512];
    const char* p = s;
    int lines = 0;
    while (*p && lines < 12) {
        int len = 0, best = 0;
        while (p[len] && len < (int)sizeof line - 1) {
            int l = len + 1;
            while (p[l] && ((unsigned char)p[l] & 0xC0) == 0x80) ++l;
            memcpy(line, p, (size_t)l);
            line[l] = 0;
            if (ui_text_w(line, size, FW_REG) > r.width) break;
            best = l;
            len = l;
        }
        if (best == 0) best = 1;
        /* prefer a break after a separator if one is close */
        if (p[best]) {
            for (int k = best; k > best * 2 / 3; --k)
                if (p[k - 1] == ' ' || p[k - 1] == '\\' || p[k - 1] == '/') { best = k; break; }
        }
        memcpy(line, p, (size_t)best);
        line[best] = 0;
        if (draw) ui_text(r.x, y, line, size, FW_REG, c);
        y += lh;
        p += best;
        lines++;
    }
    return y - r.y;
}

static void prop_row(Rectangle* area, float* y, const char* label, const char* value)
{
    float lw = 128;
    Rectangle lr = { area->x, *y, lw, 24 };
    ui_text_fit(lr, label, 12.5f, FW_REG, T.dim, AL_LEFT);
    Rectangle vr = { area->x + lw, *y + 3, area->width - lw, 24 };
    float h = 24;
    if (ui_text_w(value, 12.5f, FW_REG) > vr.width) h = wrap_text(vr, value, 12.5f, T.text, true) + 6;
    else ui_text_fit((Rectangle){ vr.x, *y, vr.width, 24 }, value, 12.5f, FW_REG, T.text, AL_LEFT);
    Rectangle full = { area->x - 6, *y, area->width + 12, h };
    if (ui_hover(full)) {
        ui_tooltip("Double-click to copy");
        if (ui_dclicked(full)) { SetClipboardText(value); toast(false, "Copied %s", label); }
    }
    *y += h;
}

static void mini_graph(Rectangle r, const char* title, const char* value, const float* ring, const Proc* p,
                       Color c, int fmt, float maxv)
{
    ui_text_fit((Rectangle){ r.x, r.y, r.width / 2, 18 }, title, 12, FW_SEMI, T.dim, AL_LEFT);
    ui_text_fit((Rectangle){ r.x + r.width / 2, r.y, r.width / 2, 18 }, value, 12.5f, FW_SEMI, c, AL_RIGHT);
    Rectangle g = { r.x, r.y + 20, r.width, r.height - 38 };
    Series s = { ring, p->first_sample, p->last_sample, c, title, true, false };
    GraphOpts o = { .maxv = maxv, .minmax = fmt == GF_PCT ? 5 : 1024, .fmt = fmt, .grid = true, .axis_labels = true, .tooltip = true };
    ui_graph(g, &s, 1, &o);
    WinStats ws = win_stats(ring, p->first_sample, p->last_sample);
    char a[32], pk[32], line[96];
    ui_format_value(fmt == GF_PCT ? GF_NUM : fmt, ws.avg, a, sizeof a);
    ui_format_value(fmt == GF_PCT ? GF_NUM : fmt, ws.peak, pk, sizeof pk);
    if (fmt == GF_PCT) snprintf(line, sizeof line, "avg %s%%   peak %s%%", a, pk);
    else snprintf(line, sizeof line, "avg %s   peak %s", a, pk);
    ui_text_fit((Rectangle){ r.x, r.y + r.height - 17, r.width, 16 }, line, 11, FW_REG, T.faint, AL_LEFT);
}

static void details_modules(Rectangle r, Proc* p)
{
    if (app.mods_pid != p->pid || app.mods_create != p->create_time) {
        if (!app.mods) app.mods = malloc(sizeof *app.mods * 4096);
        app.nmods = app.mods ? sys_modules(p->pid, app.mods, 4096) : -1;
        app.mods_pid = p->pid;
        app.mods_create = p->create_time;
        app.mods_scroll = 0;
    }
    Rectangle top = rect_cut_top(&r, 34);
    char b[64];
    if (app.nmods >= 0) snprintf(b, sizeof b, "%d modules loaded", app.nmods);
    else snprintf(b, sizeof b, "Modules unavailable (access denied)");
    ui_text_fit((Rectangle){ top.x, top.y, top.width - 90, 28 }, b, 12.5f, FW_REG, T.dim, AL_LEFT);
    if (ui_button((Rectangle){ top.x + top.width - 84, top.y, 84, 28 }, "Refresh", IC_REFRESH, BTN_SUBTLE, true))
        app.mods_pid = 0;
    if (app.nmods <= 0) { empty_state(r, IC_LIST, "Nothing to show"); return; }
    const float RH = 42;
    int first = table_scroll(r, &app.mods_scroll, app.nmods, RH, ui_id("mods", 0));
    ui_scissor_push(r);
    for (int i = first; i < app.nmods; ++i) {
        float y = r.y + i * RH - app.mods_scroll;
        if (y > r.y + r.height) break;
        Rectangle rr = { r.x, y, r.width - 12, RH };
        bool hov = ui_hover(rr);
        if (hov) {
            ui_rrect(rr, 6, T.hover);
            ui_tooltip(app.mods[i].path);
            if (ui_dclicked(rr)) sys_open_location(app.mods[i].path);
        }
        char sz[32];
        fmt_bytes((double)app.mods[i].size, sz, sizeof sz);
        ui_text_fit((Rectangle){ rr.x + 8, y + 3, rr.width - 90, 20 }, app.mods[i].name, 13, FW_REG, T.text, AL_LEFT);
        ui_text_fit((Rectangle){ rr.x + rr.width - 86, y + 3, 78, 20 }, sz, 12, FW_REG, T.dim, AL_RIGHT);
        ui_text_fit((Rectangle){ rr.x + 8, y + 21, rr.width - 16, 18 }, app.mods[i].path, 11, FW_REG, T.faint, AL_LEFT);
    }
    ui_scissor_pop();
}

static void draw_details(Rectangle r)
{
    card(r);
    Proc* p = proc_selected();
    Rectangle in = rect_inset(r, 16, 14);
    if (!p) { empty_state(in, IC_INFO, "Select a process to see its details"); return; }

    /* header */
    Rectangle head = rect_cut_top(&in, 44);
    proc_icon(p, head.x + 16, head.y + 18, 30);
    if (ui_icon_button((Rectangle){ head.x + head.width - 28, head.y + 2, 28, 28 }, IC_CANCEL, "Close details", false))
        app.details_open = false;
    ui_text_fit((Rectangle){ head.x + 42, head.y, head.width - 76, 22 }, proc_display_name(p), 15, FW_SEMI, T.text, AL_LEFT);
    char sub[160];
    snprintf(sub, sizeof sub, "%s  \xC2\xB7  PID %u%s", p->name, p->pid, p->alive ? "" : "  \xC2\xB7  exited");
    ui_text_fit((Rectangle){ head.x + 42, head.y + 21, head.width - 76, 18 }, sub, 12, FW_REG, p->alive ? T.dim : T.warn, AL_LEFT);

    /* actions */
    Rectangle act = rect_cut_top(&in, 40);
    float bx = act.x;
    float w1 = ui_button_w("End task", IC_CANCEL);
    if (ui_button((Rectangle){ bx, act.y + 4, w1, 30 }, "End task", IC_CANCEL, BTN_DANGER, p->alive))
        proc_action(PA_END, p->pid, p->create_time);
    bx += w1 + 8;
    const char* sl = p->is_suspended ? "Resume" : "Suspend";
    float w2 = ui_button_w(sl, IC_PAUSE);
    if (ui_button((Rectangle){ bx, act.y + 4, w2, 30 }, sl, p->is_suspended ? IC_PLAY : IC_PAUSE, BTN_SUBTLE, p->alive))
        proc_action(p->is_suspended ? PA_RESUME : PA_SUSPEND, p->pid, p->create_time);
    bx += w2 + 8;
    Rectangle more = { bx, act.y + 4, ui_button_w("More", IC_CHEV_D), 30 };
    if (ui_button(more, "More", IC_CHEV_D, BTN_SUBTLE, true))
        proc_menu_open(p, (Vector2){ more.x, more.y + more.height + 4 });

    static const char* tabs[] = { "Overview", "Modules" };
    Rectangle tr = rect_cut_top(&in, 40);
    ui_segmented((Rectangle){ tr.x, tr.y + 4, 200, 30 }, tabs, 2, &app.details_tab);
    in.y += 4; in.height -= 4;

    if (app.details_tab == 1) { details_modules(in, p); return; }

    /* refresh the on-demand extras once a second */
    if (app.extra_pid != p->pid || ui.time - app.extra_time > 1.0) {
        sys_proc_extra(p->pid, &app.extra);
        app.extra_pid = p->pid;
        app.extra_time = ui.time;
    }
    ProcExtra* ex = &app.extra;

    /* scrollable overview */
    Rectangle view = in;
    ui_wheel(view, &app.details_scroll, 48);
    static float content_h = 1000;
    float maxs = fmaxf(0, content_h - view.height);
    if (app.details_scroll > maxs) app.details_scroll = maxs;
    if (app.details_scroll < 0) app.details_scroll = 0;
    ui_scissor_push(view);
    Rectangle area = { view.x, view.y - app.details_scroll, view.width - 12, 0 };
    float y = area.y;

    char v[64], v2[64];
    float gw = (area.width - 14) / 2;
    snprintf(v, sizeof v, "%.1f%%", p->cpu);
    mini_graph((Rectangle){ area.x, y, gw, 118 }, "CPU", v, p->h_cpu, p, T.cpu, GF_PCT, 0);
    fmt_bytes((double)p->ws_private, v, sizeof v);
    mini_graph((Rectangle){ area.x + gw + 14, y, gw, 118 }, "Memory", v, p->h_mem, p, T.mem, GF_BYTES, 0);
    y += 128;
    fmt_rate(p->io_read_bps + p->io_write_bps, v, sizeof v);
    mini_graph((Rectangle){ area.x, y, gw, 118 }, "Disk I/O", v, p->h_io, p, T.disk, GF_RATE, 0);
    snprintf(v, sizeof v, "%.1f%%", p->gpu);
    mini_graph((Rectangle){ area.x + gw + 14, y, gw, 118 }, "GPU", v, p->h_gpu, p, T.gpu, GF_PCT, 0);
    y += 134;

    DrawRectangleRec((Rectangle){ area.x, y, area.width, 1 }, T.border);
    y += 10;
    Rectangle pa = { area.x, 0, area.width, 0 };
    prop_row(&pa, &y, "Status", !p->alive ? "Exited" : p->is_suspended ? "Suspended" : (ex->efficiency ? "Running (efficiency mode)" : "Running"));
    snprintf(v, sizeof v, "%u", p->pid);
    prop_row(&pa, &y, "PID", v);
    Proc* par = proc_find_alive(p->ppid);
    if (par && par->create_time <= p->create_time) snprintf(v, sizeof v, "%s (%u)", par->name, p->ppid);
    else snprintf(v, sizeof v, "%u (exited)", p->ppid);
    prop_row(&pa, &y, "Parent", v);
    if (p->company[0]) prop_row(&pa, &y, "Publisher", p->company);
    prop_row(&pa, &y, "User", p->user[0] ? p->user : "\xE2\x80\x94");
    snprintf(v, sizeof v, "%u", p->session);
    prop_row(&pa, &y, "Session", v);
    sys_format_filetime(p->create_time, v, sizeof v);
    prop_row(&pa, &y, "Started", v);
    fmt_dur((double)p->cpu_time / 1e7, v, sizeof v);
    prop_row(&pa, &y, "CPU time", v);
    {
        const char* pc = "\xE2\x80\x94";
        for (int i = 0; i < 6; ++i) if (ex->prio_class == PRIO_CLASSES[i].cls) pc = PRIO_CLASSES[i].name;
        snprintf(v, sizeof v, "%s (base %d)", ex->ok ? pc : prio_name(p->base_prio), p->base_prio);
        prop_row(&pa, &y, "Priority", v);
    }
    if (ex->ok) {
        int on = 0, all = 0;
        for (int i = 0; i < 64; ++i) {
            if (ex->sys_affinity & (1ULL << i)) all++;
            if (ex->affinity & (1ULL << i)) on++;
        }
        snprintf(v, sizeof v, "%d of %d logical processors", on, all);
        prop_row(&pa, &y, "Affinity", v);
        prop_row(&pa, &y, "Architecture", ex->wow64 ? "x86 (32-bit)" : "x64");
    }
    snprintf(v, sizeof v, "%u", p->threads);
    prop_row(&pa, &y, "Threads", v);
    snprintf(v, sizeof v, "%u", p->handles);
    prop_row(&pa, &y, "Handles", v);
    if (ex->ok) {
        snprintf(v, sizeof v, "%u GDI \xC2\xB7 %u USER", ex->gdi, ex->user_objs);
        prop_row(&pa, &y, "GUI objects", v);
    }
    fmt_bytes((double)p->ws, v, sizeof v);
    fmt_bytes((double)p->ws_peak, v2, sizeof v2);
    char line[160];
    snprintf(line, sizeof line, "%s (peak %s)", v, v2);
    prop_row(&pa, &y, "Working set", line);
    fmt_bytes((double)p->ws_private, v, sizeof v);
    prop_row(&pa, &y, "Private WS", v);
    fmt_bytes((double)(p->ws > p->ws_private ? p->ws - p->ws_private : 0), v, sizeof v);
    prop_row(&pa, &y, "Shareable", v);
    fmt_bytes((double)p->commit, v, sizeof v);
    prop_row(&pa, &y, "Commit size", v);
    fmt_bytes((double)p->virt, v, sizeof v);
    prop_row(&pa, &y, "Virtual size", v);
    fmt_bytes((double)p->paged_pool, v, sizeof v);
    fmt_bytes((double)p->nonpaged_pool, v2, sizeof v2);
    snprintf(line, sizeof line, "%s paged \xC2\xB7 %s non-paged", v, v2);
    prop_row(&pa, &y, "Kernel pools", line);
    snprintf(line, sizeof line, "%u total \xC2\xB7 %.0f hard/s", p->page_faults, p->hard_faults_ps);
    prop_row(&pa, &y, "Page faults", line);
    fmt_bytes((double)p->io_read, v, sizeof v);
    fmt_bytes((double)p->io_write, v2, sizeof v2);
    snprintf(line, sizeof line, "%s read \xC2\xB7 %s written", v, v2);
    prop_row(&pa, &y, "I/O total", line);
    fmt_bytes((double)p->io_other, v, sizeof v);
    prop_row(&pa, &y, "I/O other", v);
    if (p->gpu_mem) { fmt_bytes((double)p->gpu_mem, v, sizeof v); prop_row(&pa, &y, "GPU memory", v); }
    if (p->title[0]) prop_row(&pa, &y, "Window", p->title);
    prop_row(&pa, &y, "Path", p->path[0] ? p->path : "Access denied");
    if (p->cmdline[0]) prop_row(&pa, &y, "Command line", p->cmdline);
    y += 8;
    if (p->path[0]) {
        float w = ui_button_w("Open file location", IC_FOLDER);
        if (ui_button((Rectangle){ area.x, y, w, 30 }, "Open file location", IC_FOLDER, BTN_SUBTLE, true))
            sys_open_location(p->path);
        float w2b = ui_button_w("Properties", IC_INFO);
        if (ui_button((Rectangle){ area.x + w + 8, y, w2b, 30 }, "Properties", IC_INFO, BTN_SUBTLE, true))
            sys_open_properties(p->path);
        y += 40;
    }
    content_h = y - area.y + 8;
    ui_scissor_pop();
    ui_scrollbar((Rectangle){ view.x + view.width - 8, view.y, 8, view.height }, &app.details_scroll, content_h,
                 view.height, ui_id("details-scroll", 0));
}

/* ---- page ---------------------------------------------------------------------------------- */

void page_processes(Rectangle r)
{
    SysState* st = app.st;
    char sub[64];
    snprintf(sub, sizeof sub, "%d running", st->total_procs);
    Rectangle tb = page_header(&r, "Processes", sub);

    uint32_t search_id = ui_id("proc-search", 0);
    float sw = fminf(260, fmaxf(140, tb.width * 0.3f));
    Rectangle sr = rect_cut_right(&tb, sw);
    if (ui_textbox(sr, app.search, sizeof app.search, search_id, "Search name, PID, user", IC_SEARCH))
        app.proc_scroll = 0;
    rect_cut_right(&tb, 10);
    static const char* views[] = { "Grouped", "Tree", "Flat" };
    Rectangle vr = rect_cut_right(&tb, 210);
    if (ui_segmented(vr, views, 3, &app.proc_view)) app.proc_scroll = 0;
    rect_cut_right(&tb, 10);

    Proc* sel = proc_selected();
    float ew = ui_button_w("End task", IC_CANCEL);
    if (tb.width > ew + 8) {
        Rectangle er = rect_cut_right(&tb, ew);
        if (ui_button(er, "End task", IC_CANCEL, BTN_DANGER, sel && sel->alive))
            proc_action(PA_END, sel->pid, sel->create_time);
        rect_cut_right(&tb, 8);
    }
    float rw = ui_button_w("Run new task", IC_RUN);
    if (tb.width > rw + 8) {
        Rectangle rr = rect_cut_right(&tb, rw);
        if (ui_button(rr, "Run new task", IC_RUN, BTN_SUBTLE, true)) open_run_dialog();
    }

    build_rows();

    /* keyboard */
    bool typing = ui.focus != 0;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!app.modal && !app.menu.open) {
        if (ctrl && IsKeyPressed(KEY_F)) ui.focus_next = search_id;
        if (!typing) {
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) move_selection(1);
            if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) move_selection(-1);
            if (IsKeyPressed(KEY_ENTER) && sel) app.details_open = !app.details_open;
            if (IsKeyPressed(KEY_DELETE) && sel && sel->alive) proc_action(PA_END, sel->pid, sel->create_time);
        }
    }

    if (app.details_open) {
        float dw = r.width > 1100 ? 400 : fmaxf(320, r.width * 0.38f);
        Rectangle d = rect_cut_right(&r, dw);
        rect_cut_right(&r, 12);
        draw_details(d);
    }
    draw_table(r);
}
