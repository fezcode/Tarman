/* Startup apps, Users, Services and Settings. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "version.h"

#define CMPNUM(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)

static int cmp_ci(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return (unsigned char)x - (unsigned char)y;
    }
}

static void letter_tile(float cx, float cy, float size, const char* name)
{
    Proc fake;
    memset(&fake, 0, sizeof fake);
    fake.icon = -1;
    snprintf(fake.name, sizeof fake.name, "%s", name);
    proc_icon(&fake, cx, cy, size);
}

/* ---- startup -------------------------------------------------------------------- */

enum { S_NAME, S_PUB, S_STATUS, S_LOC, S_CMD, S_COUNT };
static const Column SCOLS[S_COUNT] = {
    { "Name", 220, AL_LEFT, 0 }, { "Publisher", 180, AL_LEFT, 2 }, { "Status", 100, AL_LEFT, 0 },
    { "Location", 170, AL_LEFT, 3 }, { "Command", 0, AL_LEFT, 1 },
};

static const char* startup_location(int src)
{
    switch (src) {
    case STARTUP_HKCU_RUN:      return "Registry \xC2\xB7 current user";
    case STARTUP_HKLM_RUN:      return "Registry \xC2\xB7 all users";
    case STARTUP_HKLM_RUN32:    return "Registry \xC2\xB7 all users (32-bit)";
    case STARTUP_USER_FOLDER:   return "Startup folder \xC2\xB7 user";
    case STARTUP_COMMON_FOLDER: return "Startup folder \xC2\xB7 all users";
    }
    return "?";
}

static int g_sidx[1024];
static int s_ssort;
static bool s_sdesc;

static int st_cmp(const void* A, const void* B)
{
    const StartupItem* a = &app.st->startup[*(const int*)A];
    const StartupItem* b = &app.st->startup[*(const int*)B];
    int r = 0;
    switch (s_ssort) {
    case S_NAME:   r = cmp_ci(a->display, b->display); break;
    case S_PUB:    r = cmp_ci(a->publisher, b->publisher); break;
    case S_STATUS: r = CMPNUM(a->enabled, b->enabled); break;
    case S_LOC:    r = CMPNUM(a->source, b->source); break;
    case S_CMD:    r = cmp_ci(a->command, b->command); break;
    }
    return s_sdesc ? -r : r;
}

static void startup_toggle(int idx, bool enable)
{
    if (idx < 0 || idx >= app.st->nstartup) return;
    StartupItem it = app.st->startup[idx];
    char err[256];
    if (sys_startup_set(&it, enable, err, sizeof err)) {
        app.st->startup[idx].enabled = enable;   /* optimistic; the next read confirms */
        toast(false, "%s %s", it.display, enable ? "enabled" : "disabled");
    } else {
        toast(true, "Couldn't change %s: %s%s", it.display, err,
              it.source != STARTUP_HKCU_RUN && it.source != STARTUP_USER_FOLDER && !app.st->elevated
                  ? " (run as administrator)" : "");
    }
}

void startup_menu_action(int id)
{
    int i = app.st_sel;
    if (i < 0 || i >= app.st->nstartup) return;
    StartupItem* it = &app.st->startup[i];
    switch (id) {
    case PA_ST_ENABLE:   startup_toggle(i, true); break;
    case PA_ST_DISABLE:  startup_toggle(i, false); break;
    case PA_ST_LOCATION: if (it->target[0]) sys_open_location(it->target); break;
    case PA_ST_SEARCH:   sys_search_online(it->display); break;
    case PA_ST_PROPS:    if (it->target[0]) sys_open_properties(it->target); break;
    }
}

void page_startup(Rectangle r)
{
    SysState* st = app.st;
    int enabled = 0;
    for (int i = 0; i < st->nstartup; ++i) enabled += st->startup[i].enabled;
    char sub[96];
    snprintf(sub, sizeof sub, "%d entries  \xC2\xB7  %d enabled", st->nstartup, enabled);
    Rectangle tb = page_header(&r, "Startup apps", sub);
    StartupItem* sel = app.st_sel >= 0 && app.st_sel < st->nstartup ? &st->startup[app.st_sel] : NULL;
    if (ui_button(rect_cut_right(&tb, ui_button_w("Refresh", IC_REFRESH)), "Refresh", IC_REFRESH, BTN_SUBTLE, true))
        sys_refresh_startup();
    rect_cut_right(&tb, 8);
    if (ui_button(rect_cut_right(&tb, ui_button_w("Disable", IC_STOP)), "Disable", IC_STOP, BTN_SUBTLE, sel && sel->enabled))
        startup_toggle(app.st_sel, false);
    rect_cut_right(&tb, 8);
    if (ui_button(rect_cut_right(&tb, ui_button_w("Enable", IC_CHECK)), "Enable", IC_CHECK, BTN_PRIMARY, sel && !sel->enabled))
        startup_toggle(app.st_sel, true);

    int n = 0;
    for (int i = 0; i < st->nstartup && n < 1024; ++i) g_sidx[n++] = i;
    s_ssort = app.st_sort;
    s_sdesc = app.st_desc;
    qsort(g_sidx, (size_t)n, sizeof g_sidx[0], st_cmp);

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    ColLayout L;
    table_layout(SCOLS, S_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 200);
    Rectangle head = rect_cut_top(&in, 32);
    table_header(head, SCOLS, &L, &app.st_sort, &app.st_desc);
    const float RH = 36;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.st_scroll, n, RH, ui_id("st-scroll", 0));
    ui_scissor_push(body);
    for (int k = first; k < n; ++k) {
        float y = body.y + k * RH - app.st_scroll;
        if (y > body.y + body.height) break;
        int i = g_sidx[k];
        StartupItem* it = &st->startup[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        bool issel = app.st_sel == i;
        if (issel) ui_rrect(rr, 6, T.sel);
        else if (hov) ui_rrect(rr, 6, T.hover);
        Rectangle nc = table_cell_rect(rr, &L, S_NAME);
        letter_tile(nc.x + 9, rr.y + RH / 2, 18, it->display);
        ui_text_fit((Rectangle){ nc.x + 26, rr.y, nc.width - 26, RH }, it->display, 13, FW_REG, T.text, AL_LEFT);
        table_cell(rr, &L, S_PUB, it->publisher[0] ? it->publisher : "\xE2\x80\x94", T.dim, FW_REG, AL_LEFT);
        if (L.vis[S_STATUS]) {
            Rectangle sc = table_cell_rect(rr, &L, S_STATUS);
            ui_badge(sc.x, rr.y + RH / 2, it->enabled ? "Enabled" : "Disabled", it->enabled ? T.good : T.faint);
        }
        table_cell(rr, &L, S_LOC, startup_location(it->source), T.dim, FW_REG, AL_LEFT);
        table_cell(rr, &L, S_CMD, it->command, T.faint, FW_REG, AL_LEFT);
        if (hov) {
            if (ui_rclicked(rr)) {
                app.st_sel = i;
                Menu* m = &app.menu;
                menu_open(m, ui.mouse, MENU_OWNER_STARTUP);
                menu_add(m, "Enable", IC_CHECK, PA_ST_ENABLE, !it->enabled, false);
                menu_add(m, "Disable", IC_STOP, PA_ST_DISABLE, it->enabled, false);
                menu_sep(m);
                menu_add(m, "Open file location", IC_FOLDER, PA_ST_LOCATION, it->target[0] != 0, false);
                menu_add(m, "Search online", IC_SEARCH, PA_ST_SEARCH, true, false);
                menu_add(m, "Properties", IC_INFO, PA_ST_PROPS, it->target[0] != 0, false);
            } else if (ui_clicked(rr)) app.st_sel = i;
            if (it->command[0]) ui_tooltip(it->command);
        }
    }
    ui_scissor_pop();
    if (!n) empty_state(body, IC_BOLT, "Reading startup entries\xE2\x80\xA6");
}

/* ---- users ----------------------------------------------------------------------- */

void users_menu_action(int id)
{
    char err[256];
    if (id == PA_SES_DISCONNECT) {
        if (sys_session_action(app.user_sel, 0, err, sizeof err)) toast(false, "Session %u disconnected", app.user_sel);
        else toast(true, "Couldn't disconnect: %s", err);
    } else if (id == PA_SES_SIGNOFF) {
        app.modal = MODAL_CONFIRM;
        app.modal_action = PA_SES_SIGNOFF;
        snprintf(app.modal_title, sizeof app.modal_title, "Sign off session %u?", app.user_sel);
        snprintf(app.modal_text, sizeof app.modal_text,
                 "Every app running in that session will close. Unsaved work will be lost.");
    }
}

void page_users(Rectangle r)
{
    SysState* st = app.st;
    char sub[64];
    snprintf(sub, sizeof sub, "%d signed in", st->nsession);
    page_header(&r, "Users", sub);

    Rectangle top = rect_cut_top(&r, fminf(22 + st->nsession * 58.0f, r.height * 0.45f));
    rect_cut_top(&r, 12);
    card(top);
    Rectangle in = rect_inset(top, 12, 10);
    bool sel_valid = false;
    for (int i = 0; i < st->nsession; ++i) if (st->sessions[i].id == app.user_sel) sel_valid = true;
    if (!sel_valid && st->nsession) app.user_sel = st->sessions[0].id;
    for (int i = 0; i < st->nsession; ++i) {
        SessionInfo* s = &st->sessions[i];
        float cpu = 0, gpu = 0;
        uint64_t mem = 0;
        int nproc = 0;
        double io = 0;
        for (int k = 0; k < st->nproc; ++k) {
            Proc* p = st->procs[k];
            if (!p->alive || p->session != s->id) continue;
            cpu += p->cpu; mem += p->ws_private; nproc++;
            io += p->io_read_bps + p->io_write_bps;
            gpu = fmaxf(gpu, p->gpu);
        }
        Rectangle rr = rect_cut_top(&in, 56);
        bool sel = app.user_sel == s->id;
        bool hov = ui_hover(rr);
        if (sel) ui_rrect(rr, 8, T.sel);
        else if (hov) ui_rrect(rr, 8, T.hover);
        DrawCircleV((Vector2){ rr.x + 28, rr.y + 28 }, 17, col_alpha(T.accent, 0.18f));
        ui_icon(rr.x + 28, rr.y + 28, IC_PERSON, 16, T.accent);
        char t[128];
        ui_text_fit((Rectangle){ rr.x + 56, rr.y + 8, 260, 22 }, s->user, 14, FW_SEMI, T.text, AL_LEFT);
        snprintf(t, sizeof t, "Session %u  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  %d processes", s->id, s->station, s->state, nproc);
        ui_text_fit((Rectangle){ rr.x + 56, rr.y + 29, 420, 18 }, t, 12, FW_REG, T.dim, AL_LEFT);
        float cx = rr.x + rr.width - 420;
        char v[32];
        snprintf(v, sizeof v, "%.1f%%", cpu);
        stat_item(cx, rr.y + 8, 90, "CPU", v, BLANK);
        fmt_bytes((double)mem, v, sizeof v);
        stat_item(cx + 100, rr.y + 8, 110, "Memory", v, BLANK);
        fmt_rate(io, v, sizeof v);
        stat_item(cx + 220, rr.y + 8, 110, "Disk", v, BLANK);
        snprintf(v, sizeof v, "%.0f%%", gpu);
        stat_item(cx + 340, rr.y + 8, 70, "GPU", v, BLANK);
        if (hov) {
            if (ui_rclicked(rr)) {
                app.user_sel = s->id;
                Menu* m = &app.menu;
                menu_open(m, ui.mouse, MENU_OWNER_SESSION);
                menu_add(m, "Disconnect", IC_LINK, PA_SES_DISCONNECT, true, false);
                menu_add(m, "Sign off", IC_POWER, PA_SES_SIGNOFF, true, false);
            } else if (ui_clicked(rr)) app.user_sel = s->id;
        }
        rect_cut_top(&in, 2);
    }

    /* the selected user's processes */
    card(r);
    Rectangle body = rect_inset(r, 1, 1);
    Rectangle h = rect_cut_top(&body, 40);
    ui_text_fit((Rectangle){ h.x + 14, h.y + 6, h.width - 28, 30 }, "Processes in this session", 14, FW_SEMI, T.text, AL_LEFT);
    static Proc* list[8192];
    int n = 0;
    for (int k = 0; k < st->nproc && n < 8192; ++k) {
        Proc* p = st->procs[k];
        if (p->alive && p->session == app.user_sel) list[n++] = p;
    }
    /* by CPU, then memory */
    for (int i = 1; i < n; ++i) {
        Proc* v = list[i];
        int j = i - 1;
        while (j >= 0 && (list[j]->cpu < v->cpu || (list[j]->cpu == v->cpu && list[j]->ws_private < v->ws_private))) {
            list[j + 1] = list[j];
            --j;
        }
        list[j + 1] = v;
    }
    const float RH = 30;
    int first = table_scroll(body, &app.user_scroll, n, RH, ui_id("user-scroll", 0));
    ui_scissor_push(body);
    for (int i = first; i < n; ++i) {
        float y = body.y + i * RH - app.user_scroll;
        if (y > body.y + body.height) break;
        Proc* p = list[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        if (hov) ui_rrect(rr, 6, T.hover);
        proc_icon(p, rr.x + 20, rr.y + RH / 2, 16);
        ui_text_fit((Rectangle){ rr.x + 36, rr.y, rr.width * 0.45f, RH }, proc_display_name(p), 13, FW_REG, T.text, AL_LEFT);
        char v[32];
        snprintf(v, sizeof v, "%.1f%%", p->cpu);
        ui_text_fit((Rectangle){ rr.x + rr.width - 300, rr.y, 80, RH }, v, 13, FW_REG, T.text, AL_RIGHT);
        fmt_bytes((double)p->ws_private, v, sizeof v);
        ui_text_fit((Rectangle){ rr.x + rr.width - 200, rr.y, 100, RH }, v, 13, FW_REG, T.text, AL_RIGHT);
        snprintf(v, sizeof v, "%u", p->pid);
        ui_text_fit((Rectangle){ rr.x + rr.width - 90, rr.y, 80, RH }, v, 13, FW_REG, T.dim, AL_RIGHT);
        if (hov && ui_rclicked(rr)) { proc_select(p); proc_menu_open(p, ui.mouse); }
    }
    ui_scissor_pop();
}

/* ---- services ------------------------------------------------------------------- */

enum { V_NAME, V_PID, V_DESC, V_STATUS, V_START, V_COUNT };
static const Column VCOLS[V_COUNT] = {
    { "Name", 200, AL_LEFT, 0 }, { "PID", 70, AL_RIGHT, 2 }, { "Description", 0, AL_LEFT, 0 },
    { "Status", 110, AL_LEFT, 0 }, { "Startup type", 180, AL_LEFT, 1 },
};

static int g_vidx[4096];
static int s_vsort;
static bool s_vdesc;

static const char* svc_state(int s)
{
    switch (s) {
    case 1: return "Stopped";
    case 2: return "Starting";
    case 3: return "Stopping";
    case 4: return "Running";
    case 5: return "Continuing";
    case 6: return "Pausing";
    case 7: return "Paused";
    }
    return "?";
}

static const char* svc_start(const ServiceInfo* s)
{
    switch (s->start_type) {
    case 0: return "Boot";
    case 1: return "System";
    case 2: return s->delayed ? "Automatic (delayed)" : "Automatic";
    case 3: return "Manual";
    case 4: return "Disabled";
    }
    return "\xE2\x80\x94";
}

static int svc_cmp(const void* A, const void* B)
{
    const ServiceInfo* a = &app.st->svcs[*(const int*)A];
    const ServiceInfo* b = &app.st->svcs[*(const int*)B];
    int r = 0;
    switch (s_vsort) {
    case V_NAME:   r = cmp_ci(a->name, b->name); break;
    case V_PID:    r = CMPNUM(a->pid, b->pid); break;
    case V_DESC:   r = cmp_ci(a->display, b->display); break;
    case V_STATUS: r = CMPNUM(a->state, b->state); break;
    case V_START:  r = CMPNUM(a->start_type, b->start_type); break;
    }
    if (s_vdesc) r = -r;
    if (!r) r = cmp_ci(a->name, b->name);
    return r;
}

static void svc_do(int op)
{
    if (!app.svc_sel[0]) return;
    char err[256];
    static const char* verbs[] = { "start", "stop", "restart" };
    if (sys_service_control(app.svc_sel, op, err, sizeof err))
        toast(false, "Asked %s to %s", app.svc_sel, verbs[op]);
    else
        toast(true, "Couldn't %s %s: %s%s", verbs[op], app.svc_sel, err, app.st->elevated ? "" : " (run as administrator)");
}

void services_menu_action(int id)
{
    switch (id) {
    case PA_SVC_START:   svc_do(0); break;
    case PA_SVC_STOP:    svc_do(1); break;
    case PA_SVC_RESTART: svc_do(2); break;
    case PA_SVC_OPEN:    sys_open_tool("services.msc"); break;
    case PA_GOTO_PROCESS:
        for (int i = 0; i < app.st->nsvc; ++i)
            if (!strcmp(app.st->svcs[i].name, app.svc_sel)) {
                Proc* p = proc_find_alive(app.st->svcs[i].pid);
                if (p) proc_action(PA_GOTO_PROCESS, p->pid, p->create_time);
            }
        break;
    }
}

void page_services(Rectangle r)
{
    SysState* st = app.st;
    int running = 0;
    for (int i = 0; i < st->nsvc; ++i) running += st->svcs[i].state == 4;
    char sub[64];
    snprintf(sub, sizeof sub, "%d services  \xC2\xB7  %d running", st->nsvc, running);
    Rectangle tb = page_header(&r, "Services", sub);
    Rectangle sr = rect_cut_right(&tb, fminf(240, tb.width));
    if (ui_textbox(sr, app.svc_search, sizeof app.svc_search, ui_id("svc-search", 0), "Search services", IC_SEARCH))
        app.svc_scroll = 0;
    rect_cut_right(&tb, 10);

    const ServiceInfo* sel = NULL;
    for (int i = 0; i < st->nsvc; ++i) if (!strcmp(st->svcs[i].name, app.svc_sel)) sel = &st->svcs[i];
    float w;
    w = ui_button_w("Open Services", IC_RUN);
    if (tb.width > w) { if (ui_button(rect_cut_right(&tb, w), "Open Services", IC_RUN, BTN_GHOST, true)) sys_open_tool("services.msc"); }
    rect_cut_right(&tb, 8);
    w = ui_button_w("Restart", IC_REFRESH);
    if (tb.width > w) { if (ui_button(rect_cut_right(&tb, w), "Restart", IC_REFRESH, BTN_SUBTLE, sel && sel->state == 4)) svc_do(2); }
    rect_cut_right(&tb, 8);
    w = ui_button_w("Stop", IC_STOP);
    if (tb.width > w) { if (ui_button(rect_cut_right(&tb, w), "Stop", IC_STOP, BTN_SUBTLE, sel && sel->state == 4)) svc_do(1); }
    rect_cut_right(&tb, 8);
    w = ui_button_w("Start", IC_PLAY);
    if (tb.width > w) { if (ui_button(rect_cut_right(&tb, w), "Start", IC_PLAY, BTN_PRIMARY, sel && sel->state == 1)) svc_do(0); }

    int n = 0;
    for (int i = 0; i < st->nsvc && n < 4096; ++i) {
        ServiceInfo* s = &st->svcs[i];
        if (app.svc_search[0] && !text_match(s->name, app.svc_search) && !text_match(s->display, app.svc_search)) continue;
        g_vidx[n++] = i;
    }
    s_vsort = app.svc_sort;
    s_vdesc = app.svc_desc;
    qsort(g_vidx, (size_t)n, sizeof g_vidx[0], svc_cmp);

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    ColLayout L;
    table_layout(VCOLS, V_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 200);
    Rectangle head = rect_cut_top(&in, 32);
    if (table_header(head, VCOLS, &L, &app.svc_sort, &app.svc_desc)) app.svc_scroll = 0;
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.svc_scroll, n, RH, ui_id("svc-scroll", 0));
    ui_scissor_push(body);
    for (int k = first; k < n; ++k) {
        float y = body.y + k * RH - app.svc_scroll;
        if (y > body.y + body.height) break;
        ServiceInfo* s = &st->svcs[g_vidx[k]];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        bool issel = !strcmp(app.svc_sel, s->name);
        if (issel) ui_rrect(rr, 6, T.sel);
        else if (hov) ui_rrect(rr, 6, T.hover);
        table_cell(rr, &L, V_NAME, s->name, T.text, FW_REG, AL_LEFT);
        char b[16];
        snprintf(b, sizeof b, "%u", s->pid);
        table_cell(rr, &L, V_PID, s->pid ? b : "", T.dim, FW_REG, AL_RIGHT);
        table_cell(rr, &L, V_DESC, s->display, T.dim, FW_REG, AL_LEFT);
        if (L.vis[V_STATUS]) {
            Rectangle c = table_cell_rect(rr, &L, V_STATUS);
            Color sc = s->state == 4 ? T.good : s->state == 1 ? T.faint : T.warn;
            DrawCircleV((Vector2){ c.x + 4, rr.y + RH / 2 }, 3.5f, sc);
            ui_text_fit((Rectangle){ c.x + 14, rr.y, c.width - 14, RH }, svc_state(s->state), 13, FW_REG, T.text, AL_LEFT);
        }
        table_cell(rr, &L, V_START, svc_start(s), T.dim, FW_REG, AL_LEFT);
        if (hov) {
            if (ui_rclicked(rr)) {
                snprintf(app.svc_sel, sizeof app.svc_sel, "%s", s->name);
                Menu* m = &app.menu;
                menu_open(m, ui.mouse, MENU_OWNER_SERVICE);
                menu_add(m, "Start", IC_PLAY, PA_SVC_START, s->state == 1, false);
                menu_add(m, "Stop", IC_STOP, PA_SVC_STOP, s->state == 4, false);
                menu_add(m, "Restart", IC_REFRESH, PA_SVC_RESTART, s->state == 4, false);
                menu_sep(m);
                menu_add(m, "Go to process", IC_PROCESSES, PA_GOTO_PROCESS, s->pid != 0, false);
                menu_add(m, "Open Services", IC_RUN, PA_SVC_OPEN, true, false);
            } else if (ui_clicked(rr)) snprintf(app.svc_sel, sizeof app.svc_sel, "%s", s->name);
        }
    }
    ui_scissor_pop();
    if (!n) empty_state(body, IC_SERVICES, st->nsvc ? "No services match" : "Reading services\xE2\x80\xA6");
}

/* ---- settings ---------------------------------------------------------------------- */

static void setting_row(Rectangle* r, const char* title, const char* help, Rectangle* control, float cw)
{
    Rectangle row = rect_cut_top(r, 62);
    DrawRectangleRec((Rectangle){ row.x, row.y + row.height - 1, row.width, 1 }, T.line);
    ui_text_fit((Rectangle){ row.x, row.y + 10, row.width - cw - 20, 22 }, title, 14, FW_SEMI, T.text, AL_LEFT);
    ui_text_fit((Rectangle){ row.x, row.y + 32, row.width - cw - 20, 18 }, help, 12, FW_REG, T.dim, AL_LEFT);
    *control = (Rectangle){ row.x + row.width - cw, row.y + 15, cw, 32 };
}

void page_settings(Rectangle r)
{
    page_header(&r, "Settings", NULL);
    Rectangle c = { r.x, r.y, fminf(r.width, 860), r.height };
    card(c);
    Rectangle in = rect_inset(c, 24, 10);
    Rectangle ctl;

    ui_text_fit(rect_cut_top(&in, 34), "Appearance", 12, FW_SEMI, T.accent, AL_LEFT);
    setting_row(&in, "Theme", "Dark is easier on the eyes for long monitoring sessions.", &ctl, 220);
    static const char* themes[] = { "Dark", "Light" };
    int th = app.dark ? 0 : 1;
    if (ui_segmented(ctl, themes, 2, &th)) { app.dark = th == 0; ui_theme(app.dark); settings_save(); }

    setting_row(&in, "Interface scale", "On top of your display scaling. Ctrl+Plus / Ctrl+Minus also work.", &ctl, 300);
    static const char* zooms[] = { "90%", "100%", "115%", "130%", "150%" };
    static const float zv[] = { 0.9f, 1.0f, 1.15f, 1.3f, 1.5f };
    int zi = 1;
    for (int i = 0; i < 5; ++i) if (fabsf(app.zoom - zv[i]) < 0.01f) zi = i;
    if (ui_segmented(ctl, zooms, 5, &zi)) { app.zoom = zv[zi]; settings_save(); }

    setting_row(&in, "Always on top", "Keep Tarman above other windows.", &ctl, 40);
    if (ui_switch(ctl, &app.topmost)) settings_save();

    setting_row(&in, "Hisashi menubar",
                menubar_connected() ? "Connected: File, Edit, View, Process and Help appear in Hisashi's menubar."
                                    : "Publish menus to Hisashi's menubar over the hoswl protocol when it runs.",
                &ctl, 40);
    if (ui_switch(ctl, &app.hoswl)) { menubar_set_enabled(app.hoswl); settings_save(); }

    rect_cut_top(&in, 10);
    ui_text_fit(rect_cut_top(&in, 34), "Monitoring", 12, FW_SEMI, T.accent, AL_LEFT);
    setting_row(&in, "Visible window", "Tarman always records 10 minutes at 1 sample/s; this only zooms the graphs.", &ctl, 220);
    static const char* wins[] = { "1 min", "5 min", "10 min" };
    if (ui_segmented(ctl, wins, 3, &app.window_idx)) settings_save();

    setting_row(&in, "Pause recording", "Freezes every graph and table. The timeline resumes where it stopped.", &ctl, 40);
    if (ui_switch(ctl, &app.paused)) sys_set_paused(app.paused);

    setting_row(&in, "Administrator", app.st->elevated
                     ? "Running elevated: every process, service and startup entry can be managed."
                     : "Some processes, services and startup entries need elevation to manage.", &ctl, 210);
    if (app.st->elevated) ui_badge(ctl.x + ctl.width - 90, ctl.y + 16, "Elevated", T.good);
    else if (ui_button(ctl, "Restart as administrator", IC_SHIELD, BTN_PRIMARY, true)) {
        if (sys_restart_elevated()) app.quit = true;
    }

    rect_cut_top(&in, 10);
    ui_text_fit(rect_cut_top(&in, 34), "About", 12, FW_SEMI, T.accent, AL_LEFT);
    char about[256];
    snprintf(about, sizeof about, "Tarman %s  \xC2\xB7  raylib %s  \xC2\xB7  %s", TARMAN_VERSION, RAYLIB_VERSION, app.st->os_name);
    ui_text_fit(rect_cut_top(&in, 24), about, 13, FW_REG, T.text, AL_LEFT);
    ui_text_fit(rect_cut_top(&in, 22),
                "Shortcuts: Ctrl+1..9 pages \xC2\xB7 Ctrl+F search \xC2\xB7 Ctrl+N run task \xC2\xB7 Del end task \xC2\xB7 Space pause \xC2\xB7 F1 about",
                12, FW_REG, T.dim, AL_LEFT);
}
