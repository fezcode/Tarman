/* Resource Monitor's Network, Disk and Memory tabs. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#define CMPNUM(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)

static const char* tcp_state(int s)
{
    static const char* const names[] = { "?", "Closed", "Listening", "SYN sent", "SYN received", "Established",
                                         "FIN wait 1", "FIN wait 2", "Close wait", "Closing", "Last ACK",
                                         "Time wait", "Delete TCB" };
    return s >= 0 && s <= 12 ? names[s] : "?";
}

static const char* pid_name(uint32_t pid, Proc** out)
{
    Proc* p = proc_find_alive(pid);
    if (out) *out = p;
    if (p) return p->name;
    return pid == 0 ? "System Idle Process" : pid == 4 ? "System" : "\xE2\x80\x94";
}

static void row_proc_name(Rectangle cell, float rh, uint32_t pid)
{
    Proc* p;
    const char* n = pid_name(pid, &p);
    if (p) proc_icon(p, cell.x + 8, cell.y + rh / 2, 16);
    ui_text_fit((Rectangle){ cell.x + 22, cell.y, cell.width - 22, rh }, n, 13, FW_REG, T.text, AL_LEFT);
}

static void row_menu(Rectangle rr, uint32_t pid)
{
    if (ui_rclicked(rr)) {
        Proc* p = proc_find_alive(pid);
        if (p) { proc_select(p); proc_menu_open(p, ui.mouse); }
    }
}

/* Mini card per device with a sparkline and a headline value. */
static void device_card(Rectangle r, const char* title, const char* value, const char* sub,
                        const Series* s, int ns, float maxv, int fmt)
{
    card(r);
    Rectangle in = rect_inset(r, 14, 10);
    ui_text_fit((Rectangle){ in.x, in.y, in.width * 0.6f, 20 }, title, 13, FW_SEMI, T.text, AL_LEFT);
    ui_text_fit((Rectangle){ in.x + in.width * 0.4f, in.y, in.width * 0.6f, 20 }, value, 13, FW_SEMI, s[0].color, AL_RIGHT);
    ui_text_fit((Rectangle){ in.x, in.y + 20, in.width, 18 }, sub, 11.5f, FW_REG, T.dim, AL_LEFT);
    Rectangle g = { in.x, in.y + 42, in.width, in.height - 42 };
    GraphOpts o = { .maxv = maxv, .minmax = fmt == GF_PCT ? 1 : 1024, .fmt = fmt, .grid = true, .axis_labels = maxv <= 0, .tooltip = true };
    ui_graph(g, s, ns, &o);
}

static Rectangle device_row(Rectangle* r, int n, int idx, float h)
{
    Rectangle row = { r->x, r->y, r->width, h };
    float w = (row.width - (n - 1) * 12) / n;
    return (Rectangle){ row.x + idx * (w + 12), row.y, w, h };
}

/* ---- network ---------------------------------------------------------------------- */

enum { N_PROC, N_PID, N_PROTO, N_LADDR, N_LPORT, N_RADDR, N_RPORT, N_STATE, N_COUNT };
static const Column NCOLS[N_COUNT] = {
    { "Process", 0, AL_LEFT, 0 }, { "PID", 64, AL_RIGHT, 4 }, { "Protocol", 76, AL_LEFT, 5 },
    { "Local address", 170, AL_LEFT, 3 }, { "Local port", 84, AL_RIGHT, 0 },
    { "Remote address", 190, AL_LEFT, 0 }, { "Remote port", 92, AL_RIGHT, 2 }, { "State", 110, AL_LEFT, 1 },
};

static ConnInfo* g_conns[16384];
static int  s_csort;
static bool s_cdesc;

static int conn_cmp(const void* A, const void* B)
{
    const ConnInfo* a = *(ConnInfo* const*)A;
    const ConnInfo* b = *(ConnInfo* const*)B;
    int r = 0;
    switch (s_csort) {
    case N_PROC:  r = strcmp(pid_name(a->pid, NULL), pid_name(b->pid, NULL)); break;
    case N_PID:   r = CMPNUM(a->pid, b->pid); break;
    case N_PROTO: r = CMPNUM(a->udp * 2 + a->v6, b->udp * 2 + b->v6); break;
    case N_LADDR: r = strcmp(a->local, b->local); break;
    case N_LPORT: r = CMPNUM(a->lport, b->lport); break;
    case N_RADDR: r = strcmp(a->remote, b->remote); break;
    case N_RPORT: r = CMPNUM(a->rport, b->rport); break;
    case N_STATE: r = CMPNUM(a->state, b->state); break;
    }
    if (s_cdesc) r = -r;
    if (!r) r = CMPNUM(a->lport, b->lport);
    return r;
}

void page_network(Rectangle r)
{
    SysState* st = app.st;
    char sub[64];
    snprintf(sub, sizeof sub, "%d adapters  \xC2\xB7  %d sockets", st->nnet, st->nconn);
    Rectangle tb = page_header(&r, "Network", sub);
    Rectangle sr = rect_cut_right(&tb, fminf(240, tb.width));
    ui_textbox(sr, app.search, sizeof app.search, ui_id("net-search", 0), "Filter process, address, port", IC_SEARCH);
    rect_cut_right(&tb, 10);
    static const char* tabs[] = { "Connections", "Listening" };
    ui_segmented(rect_cut_right(&tb, 220), tabs, 2, &app.net_tab);

    if (st->nnet) {
        Rectangle row = rect_cut_top(&r, 150);
        int n = st->nnet > 4 ? 4 : st->nnet;
        for (int i = 0; i < n; ++i) {
            NetInfo* a = &st->net[i];
            char v[48], s1[32], s2[32], subl[160];
            fmt_bits(a->recv_bps + a->send_bps, v, sizeof v);
            fmt_bits(a->recv_bps, s1, sizeof s1);
            fmt_bits(a->send_bps, s2, sizeof s2);
            snprintf(subl, sizeof subl, "\xE2\x86\x93 %s   \xE2\x86\x91 %s   \xC2\xB7  %s", s1, s2, a->ipv4[0] ? a->ipv4 : a->desc);
            Series s[2] = { { a->h_recv.v, 0, TL.latest, T.net, "Receive", true, false },
                            { a->h_send.v, 0, TL.latest, col_mix(T.net, T.bad, 0.45f), "Send", true, false } };
            device_card(device_row(&row, n, i, 150), a->name, v, subl, s, 2, 0, GF_BITS);
        }
        rect_cut_top(&r, 12);
    }

    /* filter */
    int n = 0;
    for (int i = 0; i < st->nconn && n < 16384; ++i) {
        ConnInfo* c = &st->conns[i];
        bool listening = c->udp || c->state == 2;
        if ((app.net_tab == 1) != listening) continue;
        if (app.search[0]) {
            char ports[16];
            snprintf(ports, sizeof ports, "%u", c->lport);
            char rports[16];
            snprintf(rports, sizeof rports, "%u", c->rport);
            if (!text_match(pid_name(c->pid, NULL), app.search) && !text_match(c->local, app.search) &&
                !text_match(c->remote, app.search) && strcmp(ports, app.search) && strcmp(rports, app.search))
                continue;
        }
        g_conns[n++] = c;
    }
    s_csort = app.conn_sort;
    s_cdesc = app.conn_desc;
    qsort(g_conns, (size_t)n, sizeof g_conns[0], conn_cmp);

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    Column cols[N_COUNT];
    memcpy(cols, NCOLS, sizeof cols);
    if (app.net_tab == 1) {
        cols[N_RADDR].prio = 20; cols[N_RPORT].prio = 21; cols[N_STATE].prio = 22;
        cols[N_LADDR].name = "Address"; cols[N_LPORT].name = "Port";
    }
    ColLayout L;
    Rectangle lay = { in.x + 4, in.y, in.width - 16, in.height };
    if (app.net_tab == 1) lay.width = fminf(lay.width, 64 + 76 + 170 + 84 + 400);
    table_layout(cols, N_COUNT, lay, &L, 180);
    if (app.net_tab == 1) { L.vis[N_RADDR] = L.vis[N_RPORT] = L.vis[N_STATE] = false; }
    Rectangle head = rect_cut_top(&in, 32);
    if (table_header(head, cols, &L, &app.conn_sort, &app.conn_desc)) app.net_scroll = 0;
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.net_scroll, n, RH, ui_id("net-scroll", 0));
    ui_scissor_push(body);
    for (int i = first; i < n; ++i) {
        float y = body.y + i * RH - app.net_scroll;
        if (y > body.y + body.height) break;
        ConnInfo* c = g_conns[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        if (hov) ui_rrect(rr, 6, T.hover);
        row_proc_name(table_cell_rect(rr, &L, N_PROC), RH, c->pid);
        char b[32];
        snprintf(b, sizeof b, "%u", c->pid);
        table_cell(rr, &L, N_PID, b, T.dim, FW_REG, AL_RIGHT);
        table_cell(rr, &L, N_PROTO, c->udp ? (c->v6 ? "UDPv6" : "UDP") : (c->v6 ? "TCPv6" : "TCP"), T.dim, FW_REG, AL_LEFT);
        table_cell(rr, &L, N_LADDR, c->local, T.text, FW_MONO, AL_LEFT);
        snprintf(b, sizeof b, "%u", c->lport);
        table_cell(rr, &L, N_LPORT, b, T.text, FW_MONO, AL_RIGHT);
        if (app.net_tab == 0) {
            table_cell(rr, &L, N_RADDR, c->remote, T.text, FW_MONO, AL_LEFT);
            snprintf(b, sizeof b, "%u", c->rport);
            table_cell(rr, &L, N_RPORT, b, T.text, FW_MONO, AL_RIGHT);
            Color sc = c->state == 5 ? T.good : (c->state >= 6 ? T.warn : T.dim);
            if (L.vis[N_STATE]) {
                Rectangle s = table_cell_rect(rr, &L, N_STATE);
                DrawCircleV((Vector2){ s.x + 4, rr.y + RH / 2 }, 3.5f, sc);
                ui_text_fit((Rectangle){ s.x + 14, rr.y, s.width - 14, RH }, tcp_state(c->state), 13, FW_REG, T.text, AL_LEFT);
            }
        }
        if (hov) row_menu(rr, c->pid);
    }
    ui_scissor_pop();
    if (!n) empty_state(body, IC_GLOBE, st->nconn ? "No sockets match" : "Reading the connection table\xE2\x80\xA6");
}

/* ---- disk ----------------------------------------------------------------------------- */

enum { D_PROC, D_PID, D_READ, D_WRITE, D_TOTAL, D_COUNT };
static const Column DCOLS[D_COUNT] = {
    { "Process", 0, AL_LEFT, 0 }, { "PID", 64, AL_RIGHT, 3 }, { "Read", 96, AL_RIGHT, 0 },
    { "Write", 96, AL_RIGHT, 0 }, { "Total", 96, AL_RIGHT, 2 },
};

static Proc* g_dprocs[8192];
static int  s_dsort;
static bool s_ddesc;

static int dproc_cmp(const void* A, const void* B)
{
    const Proc* a = *(Proc* const*)A;
    const Proc* b = *(Proc* const*)B;
    int r = 0;
    switch (s_dsort) {
    case D_PROC:  r = strcmp(a->name, b->name); break;
    case D_PID:   r = CMPNUM(a->pid, b->pid); break;
    case D_READ:  r = CMPNUM(a->io_read_bps, b->io_read_bps); break;
    case D_WRITE: r = CMPNUM(a->io_write_bps, b->io_write_bps); break;
    case D_TOTAL: r = CMPNUM(a->io_read_bps + a->io_write_bps, b->io_read_bps + b->io_write_bps); break;
    }
    if (s_ddesc) r = -r;
    if (!r) r = CMPNUM(a->pid, b->pid);
    return r;
}

static void volumes_card(Rectangle r)
{
    SysState* st = app.st;
    card(r);
    Rectangle in = rect_inset(r, 16, 12);
    Rectangle h = rect_cut_top(&in, 30);
    ui_text_fit(h, "Storage", 14, FW_SEMI, T.text, AL_LEFT);
    const float RH = 56;
    int first = table_scroll(in, &app.vol_scroll, st->nvol, RH, ui_id("vol-scroll", 0));
    ui_scissor_push(in);
    for (int i = first; i < st->nvol; ++i) {
        VolumeInfo* v = &st->vol[i];
        float y = in.y + i * RH - app.vol_scroll;
        if (y > in.y + in.height) break;
        Rectangle rr = { in.x, y, in.width - 14, RH - 6 };
        if (ui_hover(rr)) {
            ui_rrect(rr, 6, T.hover);
            if (ui_dclicked(rr)) sys_open_tool(v->root);
        }
        ui_icon(rr.x + 16, rr.y + 18, v->kind == 2 ? IC_GLOBE : IC_DISK, 16, T.dim);
        char t[128], b1[32], b2[32];
        snprintf(t, sizeof t, "%s%s%s", v->label[0] ? v->label : "Local Disk", " (", v->root);
        size_t tl = strlen(t);
        if (tl && t[tl - 1] == '\\') t[tl - 1] = ')';
        ui_text_fit((Rectangle){ rr.x + 36, rr.y + 4, rr.width * 0.55f, 22 }, t, 13, FW_SEMI, T.text, AL_LEFT);
        if (v->total) {
            fmt_bytes((double)v->free, b1, sizeof b1);
            fmt_bytes((double)v->total, b2, sizeof b2);
            snprintf(t, sizeof t, "%s free of %s  \xC2\xB7  %s", b1, b2, v->fs);
            ui_text_fit((Rectangle){ rr.x + rr.width * 0.45f, rr.y + 4, rr.width * 0.55f - 8, 22 }, t, 12, FW_REG, T.dim, AL_RIGHT);
            float used = 1.0f - (float)((double)v->free / v->total);
            ui_bar((Rectangle){ rr.x + 36, rr.y + 32, rr.width - 44, 6 }, used, used > 0.9f ? T.bad : T.disk);
        } else {
            ui_text_fit((Rectangle){ rr.x + rr.width * 0.45f, rr.y + 4, rr.width * 0.55f - 8, 22 },
                        v->kind == 2 ? "Network drive" : v->kind == 3 ? "Optical drive" : "Not ready", 12, FW_REG, T.dim, AL_RIGHT);
        }
    }
    ui_scissor_pop();
}

void page_disk(Rectangle r)
{
    SysState* st = app.st;
    char sub[64];
    snprintf(sub, sizeof sub, "%d physical disks  \xC2\xB7  %d volumes", st->ndisk, st->nvol);
    page_header(&r, "Disk", sub);

    if (st->ndisk) {
        Rectangle row = rect_cut_top(&r, 150);
        int n = st->ndisk > 4 ? 4 : st->ndisk;
        for (int i = 0; i < n; ++i) {
            DiskInfo* d = &st->disk[i];
            char t[96], v[32], s1[32], s2[32], subl[160];
            snprintf(t, sizeof t, "%s%s%s%s", d->name, d->letters[0] ? " (" : "", d->letters, d->letters[0] ? ")" : "");
            snprintf(v, sizeof v, "%.0f%% active", d->active);
            fmt_rate(d->read_bps, s1, sizeof s1);
            fmt_rate(d->write_bps, s2, sizeof s2);
            snprintf(subl, sizeof subl, "R %s  \xC2\xB7  W %s  \xC2\xB7  %.1f ms  \xC2\xB7  %s", s1, s2, d->resp_ms, d->kind);
            Series s = { d->h_active.v, 0, TL.latest, T.disk, "Active", true, false };
            device_card(device_row(&row, n, i, 150), t, v, subl, &s, 1, 100, GF_PCT);
        }
        rect_cut_top(&r, 12);
    }

    Rectangle right = rect_cut_right(&r, fminf(420, r.width * 0.4f));
    rect_cut_right(&r, 12);
    volumes_card(right);

    int n = 0;
    for (int i = 0; i < st->nproc && n < 8192; ++i) {
        Proc* p = st->procs[i];
        if (p->alive && (p->io_read_bps + p->io_write_bps) > 0) g_dprocs[n++] = p;
    }
    s_dsort = app.disk_sort;
    s_ddesc = app.disk_desc;
    qsort(g_dprocs, (size_t)n, sizeof g_dprocs[0], dproc_cmp);

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    Rectangle title = rect_cut_top(&in, 40);
    ui_text_fit((Rectangle){ title.x + 14, title.y + 6, title.width - 28, 30 }, "Processes with I/O activity", 14, FW_SEMI, T.text, AL_LEFT);
    ColLayout L;
    table_layout(DCOLS, D_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 160);
    Rectangle head = rect_cut_top(&in, 32);
    if (table_header(head, DCOLS, &L, &app.disk_sort, &app.disk_desc)) app.disk_scroll = 0;
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.disk_scroll, n, RH, ui_id("disk-scroll", 0));
    ui_scissor_push(body);
    for (int i = first; i < n; ++i) {
        float y = body.y + i * RH - app.disk_scroll;
        if (y > body.y + body.height) break;
        Proc* p = g_dprocs[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        if (hov) ui_rrect(rr, 6, T.hover);
        Rectangle nc = table_cell_rect(rr, &L, D_PROC);
        proc_icon(p, nc.x + 8, rr.y + RH / 2, 16);
        ui_text_fit((Rectangle){ nc.x + 22, rr.y, nc.width - 22, RH }, proc_display_name(p), 13, FW_REG, T.text, AL_LEFT);
        char b[32];
        snprintf(b, sizeof b, "%u", p->pid);
        table_cell(rr, &L, D_PID, b, T.dim, FW_REG, AL_RIGHT);
        fmt_rate(p->io_read_bps, b, sizeof b);
        table_cell(rr, &L, D_READ, b, T.text, FW_REG, AL_RIGHT);
        fmt_rate(p->io_write_bps, b, sizeof b);
        table_cell(rr, &L, D_WRITE, b, T.text, FW_REG, AL_RIGHT);
        fmt_rate(p->io_read_bps + p->io_write_bps, b, sizeof b);
        table_cell(rr, &L, D_TOTAL, b, T.text, FW_SEMI, AL_RIGHT);
        if (hov) row_menu(rr, p->pid);
    }
    ui_scissor_pop();
    if (!n) empty_state(body, IC_DISK, "No process is doing I/O right now");
}

/* ---- memory -------------------------------------------------------------------------------- */

enum { M_PROC, M_PID, M_HF, M_COMMIT, M_WS, M_SHARE, M_PRIV, M_COUNT };
static const Column MCOLS[M_COUNT] = {
    { "Process", 0, AL_LEFT, 0 }, { "PID", 64, AL_RIGHT, 4 }, { "Hard faults/s", 104, AL_RIGHT, 3 },
    { "Commit", 100, AL_RIGHT, 2 }, { "Working set", 104, AL_RIGHT, 0 }, { "Shareable", 96, AL_RIGHT, 5 },
    { "Private", 100, AL_RIGHT, 0 },
};

static Proc* g_mprocs[8192];
static int  s_msort;
static bool s_mdesc;

static int mproc_cmp(const void* A, const void* B)
{
    const Proc* a = *(Proc* const*)A;
    const Proc* b = *(Proc* const*)B;
    int r = 0;
    switch (s_msort) {
    case M_PROC:   r = strcmp(a->name, b->name); break;
    case M_PID:    r = CMPNUM(a->pid, b->pid); break;
    case M_HF:     r = CMPNUM(a->hard_faults_ps, b->hard_faults_ps); break;
    case M_COMMIT: r = CMPNUM(a->commit, b->commit); break;
    case M_WS:     r = CMPNUM(a->ws, b->ws); break;
    case M_SHARE:  r = CMPNUM(a->ws - a->ws_private, b->ws - b->ws_private); break;
    case M_PRIV:   r = CMPNUM(a->ws_private, b->ws_private); break;
    }
    if (s_mdesc) r = -r;
    if (!r) r = CMPNUM(a->pid, b->pid);
    return r;
}

static void physical_bar(Rectangle r, MemInfo* m)
{
    uint64_t installed = m->installed > m->total ? m->installed : m->total;
    uint64_t in_use = m->total > m->free + m->standby + m->modified ? m->total - m->free - m->standby - m->modified : m->used;
    struct { const char* name; uint64_t v; Color c; } seg[] = {
        { "Hardware reserved", m->hw_reserved, col_alpha(T.faint, 0.7f) },
        { "In use", in_use, T.mem },
        { "Modified", m->modified, col_mix(T.mem, T.warn, 0.6f) },
        { "Standby", m->standby, col_alpha(T.mem, 0.45f) },
        { "Free", m->free, col_alpha(T.faint, 0.3f) },
    };
    Rectangle bar = rect_cut_top(&r, 30);
    float x = bar.x;
    for (int i = 0; i < 5; ++i) {
        float w = bar.width * (float)((double)seg[i].v / (installed ? installed : 1));
        if (i == 4) w = bar.x + bar.width - x;
        if (w <= 0) continue;
        Rectangle s = { x, bar.y, w, bar.height };
        ui_scissor_push(s);
        ui_rrect(bar, 7, seg[i].c);
        ui_scissor_pop();
        if (i) DrawRectangleRec((Rectangle){ x, bar.y, 1.5f, bar.height }, T.panel);
        if (ui_hover(s)) {
            char b[32], tip[96];
            fmt_bytes((double)seg[i].v, b, sizeof b);
            snprintf(tip, sizeof tip, "%s\n%s", seg[i].name, b);
            ui_tooltip(tip);
        }
        x += w;
    }
    float cw = r.width / 5;
    for (int i = 0; i < 5; ++i) {
        char b[32];
        fmt_bytes((double)seg[i].v, b, sizeof b);
        float cx = r.x + cw * i;
        DrawRectangleRec((Rectangle){ cx, r.y + 14, 10, 10 }, seg[i].c);
        ui_text_fit((Rectangle){ cx + 16, r.y + 10, cw - 20, 18 }, seg[i].name, 12, FW_REG, T.dim, AL_LEFT);
        ui_text_fit((Rectangle){ cx + 16, r.y + 28, cw - 20, 22 }, b, 15, FW_SEMI, T.text, AL_LEFT);
    }
}

void page_memory(Rectangle r)
{
    SysState* st = app.st;
    MemInfo* m = &st->mem;
    char sub[96], a[32], b[32];
    fmt_bytes((double)m->used, a, sizeof a);
    fmt_bytes((double)m->total, b, sizeof b);
    snprintf(sub, sizeof sub, "%s of %s in use (%.0f%%)", a, b, m->load);
    Rectangle tb = page_header(&r, "Memory", sub);
    Rectangle sr = rect_cut_right(&tb, fminf(240, tb.width));
    ui_textbox(sr, app.search, sizeof app.search, ui_id("mem-search", 0), "Filter processes", IC_SEARCH);

    Rectangle top = rect_cut_top(&r, 110);
    card(top);
    Rectangle tin = rect_inset(top, 16, 14);
    Rectangle tl = rect_cut_top(&tin, 22);
    ui_text_fit(tl, "Physical memory", 13, FW_SEMI, T.text, AL_LEFT);
    physical_bar(tin, m);
    rect_cut_top(&r, 12);

    Rectangle row = rect_cut_top(&r, 140);
    char v[48];
    snprintf(v, sizeof v, "%.0f%%", m->load);
    Series s1 = { m->h_used.v, 0, TL.latest, T.mem, "In use", true, false };
    device_card(device_row(&row, 3, 0, 140), "Used physical memory", v, "share of usable RAM", &s1, 1, (float)m->total, GF_BYTES);
    fmt_bytes((double)m->commit, a, sizeof a);
    fmt_bytes((double)m->commit_limit, b, sizeof b);
    char cs[80];
    snprintf(cs, sizeof cs, "%s of %s limit", a, b);
    snprintf(v, sizeof v, "%.0f%%", m->commit_limit ? (double)m->commit / m->commit_limit * 100 : 0);
    Series s2 = { m->h_commit.v, 0, TL.latest, col_mix(T.mem, T.cpu, 0.4f), "Committed", true, false };
    device_card(device_row(&row, 3, 1, 140), "Commit charge", v, cs, &s2, 1, (float)m->commit_limit, GF_BYTES);
    snprintf(v, sizeof v, "%.0f /s", m->hard_faults_ps);
    Series s3 = { m->h_hard_faults.v, 0, TL.latest, T.warn, "Hard faults/s", true, false };
    device_card(device_row(&row, 3, 2, 140), "Hard faults", v, "pages read from disk", &s3, 1, 0, GF_NUM);
    rect_cut_top(&r, 12);

    int n = 0;
    for (int i = 0; i < st->nproc && n < 8192; ++i) {
        Proc* p = st->procs[i];
        if (!p->alive) continue;
        if (app.search[0] && !text_match(p->name, app.search) && !text_match(p->desc, app.search)) continue;
        g_mprocs[n++] = p;
    }
    s_msort = app.mem_sort;
    s_mdesc = app.mem_desc;
    qsort(g_mprocs, (size_t)n, sizeof g_mprocs[0], mproc_cmp);

    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    ColLayout L;
    table_layout(MCOLS, M_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 180);
    Rectangle head = rect_cut_top(&in, 32);
    if (table_header(head, MCOLS, &L, &app.mem_sort, &app.mem_desc)) app.mem_scroll = 0;
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.mem_scroll, n, RH, ui_id("mem-scroll", 0));
    ui_scissor_push(body);
    for (int i = first; i < n; ++i) {
        float y = body.y + i * RH - app.mem_scroll;
        if (y > body.y + body.height) break;
        Proc* p = g_mprocs[i];
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        if (hov) ui_rrect(rr, 6, T.hover);
        Rectangle nc = table_cell_rect(rr, &L, M_PROC);
        proc_icon(p, nc.x + 8, rr.y + RH / 2, 16);
        ui_text_fit((Rectangle){ nc.x + 22, rr.y, nc.width - 22, RH }, proc_display_name(p), 13, FW_REG, T.text, AL_LEFT);
        char bb[32];
        snprintf(bb, sizeof bb, "%u", p->pid);
        table_cell(rr, &L, M_PID, bb, T.dim, FW_REG, AL_RIGHT);
        snprintf(bb, sizeof bb, "%.0f", p->hard_faults_ps);
        table_cell(rr, &L, M_HF, bb, p->hard_faults_ps > 0 ? T.warn : T.dim, FW_REG, AL_RIGHT);
        fmt_bytes((double)p->commit, bb, sizeof bb);
        table_cell(rr, &L, M_COMMIT, bb, T.text, FW_REG, AL_RIGHT);
        fmt_bytes((double)p->ws, bb, sizeof bb);
        table_cell(rr, &L, M_WS, bb, T.text, FW_REG, AL_RIGHT);
        fmt_bytes((double)(p->ws > p->ws_private ? p->ws - p->ws_private : 0), bb, sizeof bb);
        table_cell(rr, &L, M_SHARE, bb, T.dim, FW_REG, AL_RIGHT);
        fmt_bytes((double)p->ws_private, bb, sizeof bb);
        table_cell(rr, &L, M_PRIV, bb, T.text, FW_SEMI, AL_RIGHT);
        if (hov) row_menu(rr, p->pid);
    }
    ui_scissor_pop();
}
