/* Events page: Tarman's own activity log and an explorer for the Windows
 * event logs (Application, System, Security, Setup and every non-empty
 * "Applications and Services" channel), in the shape of Event Viewer: a log
 * list, level and time filters, a table and a details pane. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static const char* const WIN_LOGS[] = { "Application", "System", "Security", "Setup" };
#define NWIN_LOGS 4

/* the table's row model: either an activity entry or an event record */
typedef struct {
    int64_t     time_ms;
    uint8_t     level;
    const char* source;
    uint32_t    id;             /* event id, or pid for activity rows */
    const char* task;
    const char* message;
    int         index;          /* into the activity ring or the event array */
} EvRow;

static EvRow*   g_rows;
static int      g_nrows, g_cap;
static uint64_t g_counts[NWIN_LOGS];
static bool     g_counts_done;
static double   g_counts_time;
static char*    g_xml;
static uint64_t g_xml_rec;

enum { EC_LEVEL, EC_TIME, EC_SOURCE, EC_ID, EC_TASK, EC_MSG, EC_COUNT };

static bool activity_mode(void) { return app.ev_source[0] == 0; }

static unsigned level_mask(void)
{
    switch (app.ev_level) {
    case 1:  return 0x06;   /* critical + error */
    case 2:  return 0x08;   /* warning */
    case 3:  return 0x11;   /* information (+ LogAlways) */
    default: return 0x3F;
    }
}

static int hours(void)
{
    static const int h[] = { 1, 24, 168, 0 };
    return h[app.ev_time];
}

static bool level_ok(uint8_t lv)
{
    unsigned m = level_mask();
    return (m >> (lv > 5 ? 4 : lv)) & 1;
}

static const char* level_name(uint8_t lv)
{
    switch (lv) {
    case 1: return "Critical";
    case 2: return "Error";
    case 3: return "Warning";
    case 5: return "Verbose";
    default: return "Information";
    }
}

static Color level_color(uint8_t lv)
{
    switch (lv) {
    case 1: return T.bad;
    case 2: return T.bad;
    case 3: return T.warn;
    case 5: return T.faint;
    default: return T.accent;
    }
}

static void push_row(EvRow r)
{
    if (g_nrows == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 1024;
        EvRow* n = realloc(g_rows, sizeof *g_rows * (size_t)g_cap);
        if (!n) return;
        g_rows = n;
    }
    g_rows[g_nrows++] = r;
}

static bool search_ok(const char* src, uint32_t id, const char* msg)
{
    if (!app.ev_search[0]) return true;
    char ids[16];
    snprintf(ids, sizeof ids, "%u", id);
    return text_match(src, app.ev_search) || (msg && text_match(msg, app.ev_search)) || !strcmp(ids, app.ev_search);
}

static void requery(void)
{
    free(g_xml);
    g_xml = NULL;
    app.ev_sel = -1;
    app.ev_scroll = 0;
    if (!activity_mode()) sys_events_query(app.ev_source, level_mask(), hours(), 3000);
}

/* ---- left: log list --------------------------------------------------------------- */

static void source_item(Rectangle* in, const char* id, const char* label, const char* count, int icon)
{
    Rectangle r = rect_cut_top(in, 34);
    bool sel = !strcmp(app.ev_source, id);
    bool hov = ui_hover(r);
    if (sel) ui_rrect(r, 6, T.sel);
    else if (hov) ui_rrect(r, 6, T.hover);
    if (sel) ui_rrect((Rectangle){ r.x, r.y + 9, 3, r.height - 18 }, 1.5f, T.accent);
    ui_icon(r.x + 18, r.y + r.height / 2, icon, 13, sel ? T.accent : T.dim);
    float cw = count ? ui_text_w(count, 11, FW_REG) + 8 : 0;
    ui_text_fit((Rectangle){ r.x + 36, r.y, r.width - 44 - cw, r.height }, label, 13, sel ? FW_SEMI : FW_REG,
                sel ? T.text : T.dim, AL_LEFT);
    if (count) ui_text_fit((Rectangle){ r.x + r.width - cw - 6, r.y, cw, r.height }, count, 11, FW_REG, T.faint, AL_RIGHT);
    if (hov) {
        ui.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (strlen(id) > 24) ui_tooltip(id);
    }
    if (ui_clicked(r) && !sel) {
        snprintf(app.ev_source, sizeof app.ev_source, "%s", id);
        requery();
    }
}

static void section(Rectangle* in, const char* title)
{
    Rectangle r = rect_cut_top(in, 30);
    ui_text_fit((Rectangle){ r.x + 6, r.y + 8, r.width, 20 }, title, 11, FW_SEMI, T.faint, AL_LEFT);
}

static void draw_sources(Rectangle r, EventsState* ev)
{
    card(r);
    Rectangle in = rect_inset(r, 8, 8);
    char cnt[32];
    section(&in, "TARMAN");
    snprintf(cnt, sizeof cnt, "%d", app.st->act_count);
    source_item(&in, "", "Activity", cnt, IC_HISTORY);
    section(&in, "WINDOWS LOGS");
    for (int i = 0; i < NWIN_LOGS; ++i) {
        fmt_count((double)g_counts[i], cnt, sizeof cnt);
        source_item(&in, WIN_LOGS[i], WIN_LOGS[i], g_counts[i] ? cnt : (i == 2 && !app.st->elevated ? "admin" : NULL),
                    i == 2 ? IC_SHIELD : IC_LIST);
    }
    section(&in, "APPLICATIONS AND SERVICES");
    if (ev->channels_busy && !ev->nchannels) {
        ui_text_fit(rect_cut_top(&in, 26), "  Scanning channels\xE2\x80\xA6", 12, FW_REG, T.faint, AL_LEFT);
        return;
    }
    const float RH = 34;
    int first = table_scroll(in, &app.ev_list_scroll, ev->nchannels, RH, ui_id("ev-chan", 0));
    ui_scissor_push(in);
    for (int i = first; i < ev->nchannels; ++i) {
        float y = in.y + i * RH - app.ev_list_scroll;
        if (y > in.y + in.height) break;
        Rectangle row = { in.x, y, in.width - 10, RH };
        const char* name = ev->channels[i].name;
        const char* label = name;
        if (!strncmp(label, "Microsoft-Windows-", 18)) label += 18;
        fmt_count((double)ev->channels[i].records, cnt, sizeof cnt);
        Rectangle tmp = row;
        source_item(&tmp, name, label, cnt, IC_LIST);
    }
    ui_scissor_pop();
}

/* ---- details pane -------------------------------------------------------------------- */

/* Word-wraps text (honouring newlines) into r starting at -scroll; returns the height. */
static float draw_wrapped(Rectangle r, const char* text, float size, int weight, Color c, float scroll)
{
    float lh = size * 1.5f, y = r.y - scroll;
    const char* p = text;
    char line[600];
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        if (seg && p[seg - 1] == '\r') seg--;
        size_t off = 0;
        if (seg == 0) y += lh;
        while (off < seg) {
            /* longest prefix that fits, broken at a space when possible */
            size_t n = seg - off, best = 0, space = 0;
            if (n > sizeof line - 1) n = sizeof line - 1;
            for (size_t k = 1; k <= n; ++k) {
                if (((unsigned char)p[off + k - 1] & 0xC0) == 0x80 && k < n) continue;
                memcpy(line, p + off, k);
                line[k] = 0;
                if (ui_text_w(line, size, weight) > r.width) break;
                best = k;
                if (p[off + k - 1] == ' ') space = k;
            }
            if (!best) best = 1;
            if (off + best < seg && space > best / 2) best = space;
            memcpy(line, p + off, best);
            line[best] = 0;
            if (y + lh > r.y - lh && y < r.y + r.height + lh) ui_text(r.x, y, line, size, weight, c);
            y += lh;
            off += best;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return y + scroll - r.y;
}

static void draw_details(Rectangle r, const EvRow* row, EventsState* ev)
{
    card(r);
    Rectangle in = rect_inset(r, 18, 12);
    Rectangle h = rect_cut_top(&in, 30);
    ui_badge(h.x, h.y + 13, level_name(row->level), level_color(row->level));
    char head[200], clock[64];
    sys_format_filetime((uint64_t)row->time_ms * 10000ULL + 116444736000000000ULL, clock, sizeof clock);
    if (activity_mode()) snprintf(head, sizeof head, "%s  \xC2\xB7  %s", row->source, clock);
    else snprintf(head, sizeof head, "%s  \xC2\xB7  Event %u  \xC2\xB7  %s", row->source, row->id, clock);
    float bx = h.x + ui_text_w(level_name(row->level), 11, FW_SEMI) + 24;
    ui_text_fit((Rectangle){ bx, h.y, h.width * 0.55f, 26 }, head, 13, FW_SEMI, T.text, AL_LEFT);

    /* actions, right-aligned */
    Rectangle tb = { h.x + h.width * 0.55f, h.y - 2, h.width * 0.45f, 30 };
    const char* msg = row->message ? row->message : "";
    float w = ui_button_w("Copy", IC_COPY);
    if (ui_button(rect_cut_right(&tb, w), "Copy", IC_COPY, BTN_SUBTLE, true)) {
        SetClipboardText(app.ev_xml && g_xml ? g_xml : msg);
        toast(false, "Copied %s", app.ev_xml ? "event XML" : "message");
    }
    if (!activity_mode()) {
        rect_cut_right(&tb, 6);
        w = ui_button_w(app.ev_xml ? "Message" : "XML", IC_LIST);
        if (ui_button(rect_cut_right(&tb, w), app.ev_xml ? "Message" : "XML", IC_LIST, BTN_SUBTLE, true)) {
            app.ev_xml = !app.ev_xml;
            app.ev_detail_scroll = 0;
        }
        rect_cut_right(&tb, 6);
        w = ui_button_w("Search online", IC_SEARCH);
        if (tb.width > w && ui_button(rect_cut_right(&tb, w), "Search online", IC_SEARCH, BTN_GHOST, true)) {
            char q[200];
            snprintf(q, sizeof q, "%s event %u", row->source, row->id);
            sys_search_online(q);
        }
    }
    Proc* p = row->id && activity_mode() ? proc_find_alive(row->id) : NULL;
    if (!activity_mode() && ev && row->index < ev->n) p = ev->recs[row->index].pid ? proc_find_alive(ev->recs[row->index].pid) : NULL;
    if (p) {
        rect_cut_right(&tb, 6);
        w = ui_button_w("Go to process", IC_PROCESSES);
        if (tb.width > w && ui_button(rect_cut_right(&tb, w), "Go to process", IC_PROCESSES, BTN_GHOST, true))
            proc_action(PA_GOTO_PROCESS, p->pid, p->create_time);
    }

    /* facts line */
    Rectangle facts = rect_cut_bottom(&in, 22);
    char f[300];
    if (activity_mode()) {
        snprintf(f, sizeof f, "Log: Tarman activity  \xC2\xB7  Category: %s%s", row->source,
                 row->id ? TextFormat("  \xC2\xB7  PID %u", row->id) : "");
    } else {
        const EventRec* e = ev && row->index < ev->n ? &ev->recs[row->index] : NULL;
        snprintf(f, sizeof f, "Log: %s  \xC2\xB7  Computer: %s  \xC2\xB7  Task: %s  \xC2\xB7  PID %u  \xC2\xB7  TID %u  \xC2\xB7  Record %llu",
                 app.ev_source, e ? e->computer : "", row->task && row->task[0] ? row->task : "None",
                 e ? e->pid : 0, e ? e->tid : 0, e ? (unsigned long long)e->record_id : 0ULL);
    }
    ui_text_fit(facts, f, 11.5f, FW_REG, T.faint, AL_LEFT);
    rect_cut_top(&in, 6);

    /* body: message or XML, scrollable */
    const char* body = msg[0] ? msg : "(This event has no message text.)";
    int weight = FW_REG;
    if (app.ev_xml && !activity_mode()) {
        const EventRec* e = ev && row->index < ev->n ? &ev->recs[row->index] : NULL;
        if (e && (!g_xml || g_xml_rec != e->record_id)) {
            free(g_xml);
            g_xml = malloc(65536);
            if (g_xml && !sys_event_xml(app.ev_source, e->record_id, g_xml, 65536)) snprintf(g_xml, 65536, "(XML unavailable)");
            g_xml_rec = e->record_id;
        }
        if (g_xml) { body = g_xml; weight = FW_MONO; }
    }
    ui_wheel(in, &app.ev_detail_scroll, 40);
    ui_scissor_push(in);
    float th = draw_wrapped((Rectangle){ in.x, in.y, in.width - 14, in.height }, body, weight == FW_MONO ? 11.5f : 13,
                            weight, T.text, app.ev_detail_scroll);
    ui_scissor_pop();
    float maxs = fmaxf(0, th - in.height);
    if (app.ev_detail_scroll > maxs) app.ev_detail_scroll = maxs;
    ui_scrollbar((Rectangle){ in.x + in.width - 8, in.y, 8, in.height }, &app.ev_detail_scroll, th, in.height,
                 ui_id("ev-detail", 0));
}

/* ---- page ------------------------------------------------------------------------------ */

static void build_rows(EventsState* ev)
{
    g_nrows = 0;
    SysState* st = app.st;
    if (activity_mode()) {
        int64_t cutoff = hours() > 0 ? sys_now_ms() - (int64_t)hours() * 3600000LL : 0;
        for (int k = 0; k < st->act_count; ++k) {
            int i = (st->act_head - 1 - k + ACTIVITY_MAX) % ACTIVITY_MAX;
            const ActivityEntry* a = &st->activity[i];
            if (a->time_ms < cutoff) break;
            if (!level_ok(a->level)) continue;
            if (!app.ev_show_proc && !strcmp(a->cat, "Process")) continue;
            if (!search_ok(a->cat, a->pid, a->text)) continue;
            push_row((EvRow){ a->time_ms, a->level, a->cat, a->pid, "", a->text, i });
        }
    } else {
        for (int i = 0; i < ev->n; ++i) {
            const EventRec* e = &ev->recs[i];
            if (!search_ok(e->provider, e->event_id, e->message)) continue;
            push_row((EvRow){ e->time_ms, e->level, e->provider, e->event_id, e->task, e->message, i });
        }
    }
}

void page_events(Rectangle r)
{
    SysState* st = app.st;
    if (!g_counts_done || ui.time - g_counts_time > 30) {
        for (int i = 0; i < NWIN_LOGS; ++i) g_counts[i] = sys_event_log_count(WIN_LOGS[i]);
        g_counts_done = true;
        g_counts_time = ui.time;
        EventsState* e = sys_events_lock();
        bool need = !e->nchannels && !e->channels_busy;
        sys_events_unlock();
        if (need) sys_events_list_channels();
        if (!activity_mode()) {
            e = sys_events_lock();
            bool stale = strcmp(e->channel, app.ev_source) != 0;
            sys_events_unlock();
            if (stale) requery();
        }
    }

    EventsState* ev = sys_events_lock();
    build_rows(ev);

    char sub[160];
    if (activity_mode()) snprintf(sub, sizeof sub, "Tarman activity  \xC2\xB7  %d shown", g_nrows);
    else snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d shown%s%s", app.ev_source, g_nrows,
                  ev->truncated ? " (newest 3,000)" : "", ev->busy ? "  \xC2\xB7  loading\xE2\x80\xA6" : "");
    Rectangle tb = page_header(&r, "Events", sub);
    Rectangle sr = rect_cut_right(&tb, fminf(240, tb.width));
    if (ui_textbox(sr, app.ev_search, sizeof app.ev_search, ui_id("ev-search", 0), "Search source, ID, message", IC_SEARCH))
        app.ev_scroll = 0;
    rect_cut_right(&tb, 8);
    float w = ui_button_w("Refresh", IC_REFRESH);
    if (tb.width > w && ui_button(rect_cut_right(&tb, w), "Refresh", IC_REFRESH, BTN_SUBTLE, true)) {
        g_counts_done = false;
        requery();
    }
    rect_cut_right(&tb, 8);
    if (activity_mode()) {
        w = ui_button_w("Log folder", IC_FOLDER);
        if (tb.width > w && ui_button(rect_cut_right(&tb, w), "Log folder", IC_FOLDER, BTN_GHOST, true)) {
            char dir[600];
            if (sys_log_dir(dir, sizeof dir)) sys_open_tool(dir);
        }
    } else {
        w = ui_button_w("Event Viewer", IC_RUN);
        if (tb.width > w && ui_button(rect_cut_right(&tb, w), "Event Viewer", IC_RUN, BTN_GHOST, true))
            sys_open_tool("eventvwr.msc");
    }

    Rectangle left = rect_cut_left(&r, fminf(270, r.width * 0.26f));
    rect_cut_left(&r, 12);
    draw_sources(left, ev);

    /* filters */
    Rectangle fb = rect_cut_top(&r, 40);
    static const char* levels[] = { "All levels", "Errors", "Warnings", "Information" };
    static const char* times[] = { "Last hour", "Last 24 h", "Last 7 days", "All time" };
    if (ui_segmented((Rectangle){ fb.x, fb.y, 380, 32 }, levels, 4, &app.ev_level)) requery();
    if (ui_segmented((Rectangle){ fb.x + 392, fb.y, 360, 32 }, times, 4, &app.ev_time)) requery();
    if (activity_mode() && fb.width > 940)
        ui_checkbox((Rectangle){ fb.x + 766, fb.y, 200, 32 }, "Process starts/exits", &app.ev_show_proc);
    rect_cut_top(&r, 4);

    if (app.ev_sel >= g_nrows) app.ev_sel = -1;
    Rectangle det = { 0 };
    if (app.ev_sel >= 0) {
        det = rect_cut_bottom(&r, fminf(260, r.height * 0.42f));
        rect_cut_bottom(&r, 12);
    }

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    Column cols[EC_COUNT] = {
        { "Level", 116, AL_LEFT, 0 },
        { "Date and time", 150, AL_LEFT, 0 },
        { activity_mode() ? "Category" : "Source", 200, AL_LEFT, 0 },
        { activity_mode() ? "PID" : "Event ID", 80, AL_RIGHT, 2 },
        { "Task category", 140, AL_LEFT, 3 },
        { "Message", 0, AL_LEFT, 0 },
    };
    if (activity_mode()) cols[EC_TASK].w = 1;   /* activity rows have no task category */
    ColLayout L;
    table_layout(cols, EC_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 200);
    if (activity_mode()) L.vis[EC_TASK] = false;
    Rectangle head = rect_cut_top(&in, 32);
    int dummy_sort = 1;
    bool dummy_desc = true;
    table_header(head, cols, &L, &dummy_sort, &dummy_desc);   /* newest first, always */
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.ev_scroll, g_nrows, RH, ui_id("ev-scroll", 0));
    ui_scissor_push(body);
    for (int i = first; i < g_nrows; ++i) {
        float y = body.y + i * RH - app.ev_scroll;
        if (y > body.y + body.height) break;
        const EvRow* row = &g_rows[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        bool sel = app.ev_sel == i;
        if (sel) ui_rrect(rr, 6, T.sel);
        else if (hov) ui_rrect(rr, 6, T.hover);
        Rectangle lc = table_cell_rect(rr, &L, EC_LEVEL);
        Color lcol = level_color(row->level);
        DrawCircleV((Vector2){ lc.x + 5, rr.y + RH / 2 }, 4, lcol);
        ui_text_fit((Rectangle){ lc.x + 16, rr.y, lc.width - 16, RH }, level_name(row->level), 13,
                    row->level <= 3 ? FW_SEMI : FW_REG, row->level <= 3 ? lcol : T.dim, AL_LEFT);
        char t[64];
        sys_format_filetime((uint64_t)row->time_ms * 10000ULL + 116444736000000000ULL, t, sizeof t);
        table_cell(rr, &L, EC_TIME, t, T.dim, FW_REG, AL_LEFT);
        table_cell(rr, &L, EC_SOURCE, row->source, T.text, FW_REG, AL_LEFT);
        char id[16];
        snprintf(id, sizeof id, "%u", row->id);
        table_cell(rr, &L, EC_ID, row->id ? id : "", T.dim, FW_REG, AL_RIGHT);
        table_cell(rr, &L, EC_TASK, row->task, T.dim, FW_REG, AL_LEFT);
        char first_line[300] = "";
        if (row->message) {
            const char* nl = strpbrk(row->message, "\r\n");
            size_t n = nl ? (size_t)(nl - row->message) : strlen(row->message);
            if (n > sizeof first_line - 1) n = sizeof first_line - 1;
            memcpy(first_line, row->message, n);
            first_line[n] = 0;
        }
        table_cell(rr, &L, EC_MSG, first_line, T.text, FW_REG, AL_LEFT);
        if (hov && ui_clicked(rr)) {
            app.ev_sel = sel ? -1 : i;
            app.ev_detail_scroll = 0;
        }
    }
    ui_scissor_pop();
    if (!g_nrows) {
        const char* why = ev->error[0] && !activity_mode() ? ev->error
                        : ev->busy && !activity_mode() ? "Reading the log\xE2\x80\xA6"
                        : "No events match these filters";
        empty_state(body, ev->error[0] ? IC_WARNING : IC_LIST, why);
    }

    if (app.ev_sel >= 0 && app.ev_sel < g_nrows) draw_details(det, &g_rows[app.ev_sel], activity_mode() ? NULL : ev);
    sys_events_unlock();
    (void)st;
}
