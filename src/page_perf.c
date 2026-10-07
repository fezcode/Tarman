/* Performance page: Task Manager's device list (CPU, memory, every disk,
 * adapter and GPU) with a live ten-minute graph and the stat panel for the
 * selected device. Every graph shares the timeline cursor. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

typedef enum { DEV_CPU, DEV_MEM, DEV_DISK, DEV_NET, DEV_GPU } DevKind;
typedef struct { DevKind kind; int idx; } Dev;

static int devices(Dev* out, int max)
{
    SysState* st = app.st;
    int n = 0;
    out[n++] = (Dev){ DEV_CPU, 0 };
    out[n++] = (Dev){ DEV_MEM, 0 };
    for (int i = 0; i < st->ndisk && n < max; ++i) out[n++] = (Dev){ DEV_DISK, i };
    for (int i = 0; i < st->nnet && n < max; ++i) out[n++] = (Dev){ DEV_NET, i };
    for (int i = 0; i < st->ngpu && n < max; ++i) out[n++] = (Dev){ DEV_GPU, i };
    return n;
}

static void kv(float x, float* y, float w, const char* label, const char* value)
{
    ui_text_fit((Rectangle){ x, *y, w * 0.5f, 22 }, label, 12.5f, FW_REG, T.dim, AL_LEFT);
    ui_text_fit((Rectangle){ x + w * 0.5f, *y, w * 0.5f, 22 }, value, 12.5f, FW_REG, T.text, AL_LEFT);
    *y += 22;
}

static void window_stats_line(Rectangle r, const char* label, const float* ring, int fmt, Color c)
{
    WinStats s = win_stats(ring, 0, TL.latest);
    char a[32], p[32], q[32], line[160];
    ui_format_value(fmt, s.avg, a, sizeof a);
    ui_format_value(fmt, s.peak, p, sizeof p);
    ui_format_value(fmt, s.p95, q, sizeof q);
    DrawCircleV((Vector2){ r.x + 4, r.y + r.height / 2 }, 3.5f, c);
    ui_text_fit((Rectangle){ r.x + 14, r.y, 120, r.height }, label, 12, FW_SEMI, T.dim, AL_LEFT);
    snprintf(line, sizeof line, "avg %s   \xC2\xB7   p95 %s   \xC2\xB7   peak %s", a, q, p);
    ui_text_fit((Rectangle){ r.x + 134, r.y, r.width - 134, r.height }, line, 12, FW_REG, T.text, AL_LEFT);
}

static const char* window_label(void)
{
    return app.window_idx == 0 ? "60 seconds" : app.window_idx == 1 ? "5 minutes" : "10 minutes";
}

/* ---- device list ----------------------------------------------------------------- */

static void dev_title(const Dev* d, char* title, int tn, char* sub, int sn, Color* c,
                      const float** ring, const float** ring2, float* maxv)
{
    SysState* st = app.st;
    char a[32], b[32];
    *ring2 = NULL;
    *maxv = 100;
    switch (d->kind) {
    case DEV_CPU:
        snprintf(title, tn, "CPU");
        if (st->cpu.temp_ok)
            snprintf(sub, sn, "%.0f%%  %.2f GHz  (%.0f \xC2\xB0" "C)", st->cpu.usage, st->cpu.mhz / 1000.0f, st->cpu.temp);
        else
            snprintf(sub, sn, "%.0f%%  %.2f GHz", st->cpu.usage, st->cpu.mhz / 1000.0f);
        *c = T.cpu; *ring = st->cpu.h_usage.v;
        break;
    case DEV_MEM:
        snprintf(title, tn, "Memory");
        fmt_bytes((double)st->mem.used, a, sizeof a);
        fmt_bytes((double)st->mem.total, b, sizeof b);
        snprintf(sub, sn, "%s / %s (%.0f%%)", a, b, st->mem.load);
        *c = T.mem; *ring = st->mem.h_used.v; *maxv = (float)st->mem.total;
        break;
    case DEV_DISK: {
        DiskInfo* k = &st->disk[d->idx];
        snprintf(title, tn, "%s%s%s%s", k->name, k->letters[0] ? " (" : "", k->letters, k->letters[0] ? ")" : "");
        snprintf(sub, sn, "%s%s%.0f%%", k->kind, k->kind[0] ? "  " : "", k->active);
        *c = T.disk; *ring = k->h_active.v;
        break;
    }
    case DEV_NET: {
        NetInfo* n = &st->net[d->idx];
        snprintf(title, tn, "%s", n->name);
        fmt_bits(n->send_bps, a, sizeof a);
        fmt_bits(n->recv_bps, b, sizeof b);
        snprintf(sub, sn, "S: %s  R: %s", a, b);
        *c = T.net; *ring = n->h_recv.v; *ring2 = n->h_send.v; *maxv = 0;
        break;
    }
    case DEV_GPU: {
        GpuInfo* g = &st->gpu[d->idx];
        snprintf(title, tn, "GPU %d", d->idx);
        if (g->temp_ok) snprintf(sub, sn, "%.0f%%  (%.0f \xC2\xB0" "C)  %s", g->util, g->temp, g->name);
        else snprintf(sub, sn, "%.0f%%  %s", g->util, g->name);
        *c = T.gpu; *ring = g->h_util.v;
        break;
    }
    }
}

static void draw_device_list(Rectangle r, Dev* devs, int n)
{
    const float RH = 70;
    int first = table_scroll(r, &app.perf_list_scroll, n, RH, ui_id("perf-list", 0));
    ui_scissor_push(r);
    for (int i = first; i < n; ++i) {
        float y = r.y + i * RH - app.perf_list_scroll;
        if (y > r.y + r.height) break;
        Rectangle rr = { r.x, y + 2, r.width - 12, RH - 4 };
        bool sel = app.perf_dev == i;
        bool hov = ui_hover(rr);
        if (sel) ui_rrect(rr, 8, T.sel);
        else if (hov) ui_rrect(rr, 8, T.hover);
        if (sel) ui_rrect((Rectangle){ rr.x, rr.y + 14, 3, rr.height - 28 }, 1.5f, T.accent);
        char title[96], sub[160];
        Color c;
        const float *ring, *ring2;
        float maxv;
        dev_title(&devs[i], title, sizeof title, sub, sizeof sub, &c, &ring, &ring2, &maxv);
        Rectangle sp = { rr.x + 10, rr.y + 10, 74, rr.height - 20 };
        ui_rrect(sp, 4, col_alpha(c, 0.06f));
        ui_rrect_line(sp, 4, 1, col_alpha(c, 0.45f));
        if (ring2) {
            Series s[2] = { { ring, 0, TL.latest, c, NULL, true, false },
                            { ring2, 0, TL.latest, col_mix(c, T.text, 0.35f), NULL, false, false } };
            GraphOpts o = { .maxv = 0, .minmax = 1024, .mini = true };
            ui_graph(rect_inset(sp, 1, 1), s, 2, &o);
        } else {
            ui_sparkline(rect_inset(sp, 1, 1), ring, 0, TL.latest, maxv, c);
        }
        ui_text_fit((Rectangle){ sp.x + sp.width + 12, rr.y + 12, rr.width - sp.width - 28, 22 }, title, 14, FW_SEMI, T.text, AL_LEFT);
        ui_text_fit((Rectangle){ sp.x + sp.width + 12, rr.y + 34, rr.width - sp.width - 28, 20 }, sub, 12, FW_REG, T.dim, AL_LEFT);
        if (hov) ui.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (ui_clicked(rr)) app.perf_dev = i;
    }
    ui_scissor_pop();
}

/* ---- detail panes ------------------------------------------------------------------- */

static Rectangle detail_header(Rectangle* r, const char* title, const char* right)
{
    Rectangle h = rect_cut_top(r, 48);
    float tw = ui_text_w(title, 24, FW_SEMI);
    ui_text_fit((Rectangle){ h.x, h.y, tw + 4, 40 }, title, 24, FW_SEMI, T.text, AL_LEFT);
    Rectangle rest = { h.x + tw + 20, h.y + 6, h.width - tw - 20, 32 };
    ui_text_fit((Rectangle){ rest.x, rest.y, rest.width, 32 }, right, 14, FW_REG, T.dim, AL_RIGHT);
    return rest;
}

static void graph_caption(Rectangle* r, const char* left, const char* right)
{
    Rectangle c = rect_cut_top(r, 22);
    ui_text_fit((Rectangle){ c.x, c.y, c.width / 2, 20 }, left, 12, FW_REG, T.dim, AL_LEFT);
    ui_text_fit((Rectangle){ c.x + c.width / 2, c.y, c.width / 2, 20 }, right, 12, FW_REG, T.dim, AL_RIGHT);
}

static void detail_cpu(Rectangle r)
{
    CpuInfo* c = &app.st->cpu;
    SysState* st = app.st;
    detail_header(&r, "CPU", c->name);
    Rectangle tools = rect_cut_top(&r, 38);
    static const char* views[] = { "Overall", "Logical processors" };
    int v = app.perf_cores ? 1 : 0;
    if (ui_segmented((Rectangle){ tools.x, tools.y, 260, 30 }, views, 2, &v)) app.perf_cores = v == 1;
    char cap[96];
    snprintf(cap, sizeof cap, "%% Utilization over %s", window_label());
    Rectangle stats = rect_cut_bottom(&r, 214);
    graph_caption(&r, cap, "100%");
    GraphOpts o = { .maxv = 100, .fmt = GF_PCT, .grid = true, .tooltip = true };
    if (!app.perf_cores || c->logical <= 1) {
        Series s[2] = { { c->h_usage.v, 0, TL.latest, T.cpu, "Total", true, false },
                        { c->h_kernel.v, 0, TL.latest, T.kernel, "Kernel", false, false } };
        ui_graph(r, s, 2, &o);
    } else {
        int n = c->logical;
        int cols = (int)ceilf(sqrtf(n * r.width / fmaxf(1, r.height) / 1.6f));
        if (cols < 1) cols = 1;
        if (cols > n) cols = n;
        int rows = (n + cols - 1) / cols;
        float gw = (r.width - (cols - 1) * 6) / cols, gh = (r.height - (rows - 1) * 6) / rows;
        GraphOpts mo = { .maxv = 100, .fmt = GF_PCT, .grid = false, .tooltip = true };
        for (int i = 0; i < n; ++i) {
            Rectangle g = { r.x + (i % cols) * (gw + 6), r.y + (i / cols) * (gh + 6), gw, gh };
            char lab[16];
            snprintf(lab, sizeof lab, "CPU %d", i);
            Series s = { c->h_core[i].v, 0, TL.latest, T.cpu, lab, true, false };
            ui_graph(g, &s, 1, &mo);
            if (gh > 40) {
                char pct[16];
                snprintf(pct, sizeof pct, "%.0f%%", c->core[i]);
                ui_text_fit((Rectangle){ g.x + 6, g.y + 2, g.width - 12, 16 }, lab, 10.5f, FW_REG, T.faint, AL_LEFT);
                ui_text_fit((Rectangle){ g.x + 6, g.y + 2, g.width - 12, 16 }, pct, 10.5f, FW_SEMI, T.dim, AL_RIGHT);
            }
        }
    }

    /* stats */
    stats.y += 14; stats.height -= 14;
    Rectangle left = rect_cut_left(&stats, stats.width * 0.58f);
    float cw = (left.width - 20) / 3;
    char b[64];
    snprintf(b, sizeof b, "%.0f%%", c->usage);
    stat_item(left.x + 8, left.y, cw, "Utilization", b, T.cpu);
    snprintf(b, sizeof b, "%.2f GHz", c->mhz / 1000.0f);
    stat_item(left.x + 8 + cw, left.y, cw, "Speed", b, BLANK);
    snprintf(b, sizeof b, "%.0f%%", c->kernel);
    stat_item(left.x + 8 + cw * 2, left.y, cw, "Kernel time", b, T.kernel);
    fmt_count(st->total_procs, b, sizeof b);
    stat_item(left.x + 8, left.y + 52, cw, "Processes", b, BLANK);
    fmt_count(st->total_threads, b, sizeof b);
    stat_item(left.x + 8 + cw, left.y + 52, cw, "Threads", b, BLANK);
    fmt_count(st->total_handles, b, sizeof b);
    stat_item(left.x + 8 + cw * 2, left.y + 52, cw, "Handles", b, BLANK);
    fmt_dur(st->uptime_ms / 1000.0, b, sizeof b);
    stat_item(left.x + 8, left.y + 104, cw, "Up time", b, BLANK);
    Rectangle tr = { left.x + 8 + cw, left.y + 104, cw * 2, 44 };
    if (c->temp_ok) {
        snprintf(b, sizeof b, "%.0f \xC2\xB0" "C", c->temp);
        stat_item(tr.x, tr.y, cw, "Temperature", b, T.warn);
        ui_text_fit((Rectangle){ tr.x + cw, tr.y + 20, cw, 20 }, c->temp_src, 11, FW_REG, T.faint, AL_LEFT);
    } else {
        stat_item(tr.x, tr.y, cw * 2, "Temperature", "Unavailable", BLANK);
    }
    if (ui_hover(tr))
        ui_tooltip(c->temp_ok ? "CPU temperature\nSource shown on the right"
                              : "Windows has no driver-free CPU temperature sensor here.\n"
                                "Run LibreHardwareMonitor or HWiNFO (shared memory on)\n"
                                "and Tarman reads the CPU package temperature from it.");
    window_stats_line((Rectangle){ left.x, left.y + 160, left.width, 22 }, "Total", c->h_usage.v, GF_PCT, T.cpu);
    window_stats_line((Rectangle){ left.x, left.y + 182, left.width, 22 }, "Kernel", c->h_kernel.v, GF_PCT, T.kernel);

    float y = stats.y;
    float x = stats.x + 16, w = stats.width - 16;
    snprintf(b, sizeof b, "%.2f GHz", c->base_mhz / 1000.0f);
    kv(x, &y, w, "Base speed", b);
    snprintf(b, sizeof b, "%d", c->sockets);
    kv(x, &y, w, "Sockets", b);
    snprintf(b, sizeof b, "%d", c->cores);
    kv(x, &y, w, "Cores", b);
    snprintf(b, sizeof b, "%d", c->logical);
    kv(x, &y, w, "Logical processors", b);
    kv(x, &y, w, "Virtualization", c->virtualization ? "Enabled" : "Disabled");
    fmt_bytes((double)c->l1, b, sizeof b);
    kv(x, &y, w, "L1 cache", b);
    fmt_bytes((double)c->l2, b, sizeof b);
    kv(x, &y, w, "L2 cache", b);
    fmt_bytes((double)c->l3, b, sizeof b);
    kv(x, &y, w, "L3 cache", b);
}

static void composition_bar(Rectangle r, MemInfo* m)
{
    uint64_t total = m->total ? m->total : 1;
    uint64_t in_use = m->total > m->free + m->standby + m->modified ? m->total - m->free - m->standby - m->modified : m->used;
    struct { const char* name; uint64_t v; Color c; const char* help; } seg[] = {
        { "In use", in_use, T.mem, "Memory used by processes, drivers and the OS" },
        { "Modified", m->modified, col_mix(T.mem, T.warn, 0.6f), "Must be written to disk before it can be reused" },
        { "Standby", m->standby, col_alpha(T.mem, 0.45f), "Cached data not in use, ready to be reused" },
        { "Free", m->free, col_alpha(T.faint, 0.35f), "Not in use; available immediately" },
    };
    ui_rrect(r, 6, col_alpha(T.faint, 0.15f));
    float x = r.x;
    for (int i = 0; i < 4; ++i) {
        float w = r.width * (float)((double)seg[i].v / total);
        if (i == 3) w = r.x + r.width - x;
        if (w <= 0) continue;
        Rectangle s = { x, r.y, w, r.height };
        ui_scissor_push(s);
        ui_rrect(r, 6, seg[i].c);
        ui_scissor_pop();
        if (i > 0) DrawRectangleRec((Rectangle){ x, r.y, 1.5f, r.height }, T.panel);
        if (ui_hover(s)) {
            char b[32], tip[200];
            fmt_bytes((double)seg[i].v, b, sizeof b);
            snprintf(tip, sizeof tip, "%s  %s\n%s", seg[i].name, b, seg[i].help);
            ui_tooltip(tip);
        }
        x += w;
    }
    float lx = r.x;
    for (int i = 0; i < 4; ++i) {
        char b[48], lab[64];
        fmt_bytes((double)seg[i].v, b, sizeof b);
        snprintf(lab, sizeof lab, "%s %s", seg[i].name, b);
        legend_dot(lx, r.y + r.height + 14, seg[i].c, lab);
        lx += ui_text_w(lab, 12, FW_REG) + 34;
    }
}

static void detail_mem(Rectangle r)
{
    MemInfo* m = &app.st->mem;
    char right[96], b[64], b2[64];
    fmt_bytes((double)m->installed ? (double)m->installed : (double)m->total, b, sizeof b);
    snprintf(right, sizeof right, "%s %s", b, m->type);
    detail_header(&r, "Memory", right);
    Rectangle stats = rect_cut_bottom(&r, 200);
    Rectangle comp = rect_cut_bottom(&r, 66);
    Rectangle commit = rect_cut_bottom(&r, 96);
    char cap[96];
    snprintf(cap, sizeof cap, "Memory usage over %s", window_label());
    fmt_bytes((double)m->total, b, sizeof b);
    graph_caption(&r, cap, b);
    r.height -= 10;
    GraphOpts o = { .maxv = (float)m->total, .fmt = GF_BYTES, .grid = true, .tooltip = true };
    Series s = { m->h_used.v, 0, TL.latest, T.mem, "In use", true, false };
    ui_graph(r, &s, 1, &o);

    fmt_bytes((double)m->commit_limit, b, sizeof b);
    graph_caption(&commit, "Committed", b);
    commit.height -= 8;
    GraphOpts oc = { .maxv = (float)m->commit_limit, .fmt = GF_BYTES, .grid = true, .tooltip = true };
    Series sc = { m->h_commit.v, 0, TL.latest, col_mix(T.mem, T.cpu, 0.4f), "Committed", true, false };
    ui_graph(commit, &sc, 1, &oc);

    Rectangle cl = rect_cut_top(&comp, 20);
    ui_text_fit(cl, "Memory composition", 12, FW_REG, T.dim, AL_LEFT);
    composition_bar((Rectangle){ comp.x, comp.y + 2, comp.width, 18 }, m);

    stats.y += 10;
    Rectangle left = rect_cut_left(&stats, stats.width * 0.58f);
    float cw = (left.width - 20) / 3;
    fmt_bytes((double)m->used, b, sizeof b);
    if (m->compressed) {
        fmt_bytes((double)m->compressed, b2, sizeof b2);
        char t[96];
        snprintf(t, sizeof t, "%s (%s)", b, b2);
        stat_item(left.x + 8, left.y, cw, "In use (Compressed)", t, T.mem);
    } else stat_item(left.x + 8, left.y, cw, "In use", b, T.mem);
    fmt_bytes((double)m->avail, b, sizeof b);
    stat_item(left.x + 8 + cw, left.y, cw, "Available", b, BLANK);
    fmt_bytes((double)m->cached, b, sizeof b);
    stat_item(left.x + 8 + cw * 2, left.y, cw, "Cached", b, BLANK);
    char t[96];
    fmt_bytes((double)m->commit, b, sizeof b);
    fmt_bytes((double)m->commit_limit, b2, sizeof b2);
    snprintf(t, sizeof t, "%s / %s", b, b2);
    stat_item(left.x + 8, left.y + 52, cw, "Committed", t, BLANK);
    fmt_bytes((double)m->paged_pool, b, sizeof b);
    stat_item(left.x + 8 + cw, left.y + 52, cw, "Paged pool", b, BLANK);
    fmt_bytes((double)m->nonpaged_pool, b, sizeof b);
    stat_item(left.x + 8 + cw * 2, left.y + 52, cw, "Non-paged pool", b, BLANK);
    snprintf(b, sizeof b, "%.0f /s", m->hard_faults_ps);
    stat_item(left.x + 8, left.y + 104, cw, "Hard faults", b, BLANK);
    window_stats_line((Rectangle){ left.x, left.y + 160, left.width, 22 }, "In use", m->h_used.v, GF_BYTES, T.mem);

    float y = stats.y, x = stats.x + 16, w = stats.width - 16;
    if (m->speed_mhz) { snprintf(b, sizeof b, "%u MT/s", m->speed_mhz); kv(x, &y, w, "Speed", b); }
    if (m->slots_total) { snprintf(b, sizeof b, "%d of %d", m->slots_used, m->slots_total); kv(x, &y, w, "Slots used", b); }
    if (m->form[0]) kv(x, &y, w, "Form factor", m->form);
    if (m->type[0]) kv(x, &y, w, "Type", m->type);
    fmt_bytes((double)m->hw_reserved, b, sizeof b);
    kv(x, &y, w, "Hardware reserved", b);
    fmt_bytes((double)m->total, b, sizeof b);
    kv(x, &y, w, "Usable", b);
}

static void detail_disk(Rectangle r, int i)
{
    DiskInfo* d = &app.st->disk[i];
    char title[96], b[64];
    snprintf(title, sizeof title, "%s%s%s%s", d->name, d->letters[0] ? " (" : "", d->letters, d->letters[0] ? ")" : "");
    detail_header(&r, title, d->model);
    Rectangle stats = rect_cut_bottom(&r, 150);
    float half = (r.height - 12) / 2;
    Rectangle g1 = rect_cut_top(&r, half);
    rect_cut_top(&r, 12);
    char cap[96];
    snprintf(cap, sizeof cap, "Active time over %s", window_label());
    graph_caption(&g1, cap, "100%");
    GraphOpts o = { .maxv = 100, .fmt = GF_PCT, .grid = true, .tooltip = true };
    Series s = { d->h_active.v, 0, TL.latest, T.disk, "Active", true, false };
    ui_graph(g1, &s, 1, &o);

    Rectangle cap2 = rect_cut_top(&r, 22);
    ui_text_fit(cap2, "Disk transfer rate", 12, FW_REG, T.dim, AL_LEFT);
    GraphOpts o2 = { .maxv = 0, .minmax = 1024 * 1024, .fmt = GF_RATE, .grid = true, .axis_labels = true, .tooltip = true };
    Series s2[2] = { { d->h_read.v, 0, TL.latest, T.disk, "Read", true, false },
                     { d->h_write.v, 0, TL.latest, col_mix(T.disk, T.cpu, 0.6f), "Write", false, false } };
    ui_graph(r, s2, 2, &o2);
    legend_dot(cap2.x + cap2.width - 140, cap2.y + 10, T.disk, "Read");
    legend_dot(cap2.x + cap2.width - 70, cap2.y + 10, col_mix(T.disk, T.cpu, 0.6f), "Write");

    stats.y += 14;
    Rectangle left = rect_cut_left(&stats, stats.width * 0.58f);
    float cw = (left.width - 20) / 3;
    snprintf(b, sizeof b, "%.0f%%", d->active);
    stat_item(left.x + 8, left.y, cw, "Active time", b, T.disk);
    snprintf(b, sizeof b, "%.1f ms", d->resp_ms);
    stat_item(left.x + 8 + cw, left.y, cw, "Avg response time", b, BLANK);
    snprintf(b, sizeof b, "%.0f", d->queue);
    stat_item(left.x + 8 + cw * 2, left.y, cw, "Queue length", b, BLANK);
    fmt_rate(d->read_bps, b, sizeof b);
    stat_item(left.x + 8, left.y + 52, cw, "Read speed", b, T.disk);
    fmt_rate(d->write_bps, b, sizeof b);
    stat_item(left.x + 8 + cw, left.y + 52, cw, "Write speed", b, col_mix(T.disk, T.cpu, 0.6f));
    window_stats_line((Rectangle){ left.x, left.y + 108, left.width, 22 }, "Active", d->h_active.v, GF_PCT, T.disk);

    float y = stats.y, x = stats.x + 16, w = stats.width - 16;
    fmt_bytes((double)d->capacity, b, sizeof b);
    kv(x, &y, w, "Capacity", d->capacity ? b : "\xE2\x80\x94");
    kv(x, &y, w, "Type", d->kind[0] ? d->kind : "\xE2\x80\x94");
    kv(x, &y, w, "System disk", d->system ? "Yes" : "No");
    kv(x, &y, w, "Volumes", d->letters[0] ? d->letters : "\xE2\x80\x94");
}

static void detail_net(Rectangle r, int i)
{
    NetInfo* n = &app.st->net[i];
    char b[64];
    detail_header(&r, n->kind, n->desc);
    Rectangle stats = rect_cut_bottom(&r, 170);
    Rectangle cap = rect_cut_top(&r, 22);
    char c[96];
    snprintf(c, sizeof c, "Throughput over %s", window_label());
    ui_text_fit(cap, c, 12, FW_REG, T.dim, AL_LEFT);
    Color sendc = col_mix(T.net, T.bad, 0.45f);
    legend_dot(cap.x + cap.width - 160, cap.y + 10, T.net, "Receive");
    legend_dot(cap.x + cap.width - 76, cap.y + 10, sendc, "Send");
    GraphOpts o = { .maxv = 0, .minmax = 1024, .fmt = GF_BITS, .grid = true, .axis_labels = true, .tooltip = true };
    Series s[2] = { { n->h_recv.v, 0, TL.latest, T.net, "Receive", true, false },
                    { n->h_send.v, 0, TL.latest, sendc, "Send", true, false } };
    ui_graph(r, s, 2, &o);

    stats.y += 14;
    Rectangle left = rect_cut_left(&stats, stats.width * 0.58f);
    float cw = (left.width - 20) / 3;
    fmt_bits(n->send_bps, b, sizeof b);
    stat_item(left.x + 8, left.y, cw, "Send", b, sendc);
    fmt_bits(n->recv_bps, b, sizeof b);
    stat_item(left.x + 8 + cw, left.y, cw, "Receive", b, T.net);
    fmt_bytes((double)(n->in_octets + n->out_octets), b, sizeof b);
    stat_item(left.x + 8 + cw * 2, left.y, cw, "Total transferred", b, BLANK);
    window_stats_line((Rectangle){ left.x, left.y + 60, left.width, 22 }, "Receive", n->h_recv.v, GF_BITS, T.net);
    window_stats_line((Rectangle){ left.x, left.y + 82, left.width, 22 }, "Send", n->h_send.v, GF_BITS, sendc);

    float y = stats.y, x = stats.x + 16, w = stats.width - 16;
    kv(x, &y, w, "Adapter name", n->name);
    kv(x, &y, w, "Connection type", n->kind);
    kv(x, &y, w, "IPv4 address", n->ipv4[0] ? n->ipv4 : "\xE2\x80\x94");
    kv(x, &y, w, "IPv6 address", n->ipv6[0] ? n->ipv6 : "\xE2\x80\x94");
    kv(x, &y, w, "Physical address", n->mac[0] ? n->mac : "\xE2\x80\x94");
    if (n->dns[0]) kv(x, &y, w, "DNS suffix", n->dns);
    fmt_bits(n->link_bps / 8.0, b, sizeof b);
    kv(x, &y, w, "Link speed", b);
}

static void detail_gpu(Rectangle r, int i)
{
    GpuInfo* g = &app.st->gpu[i];
    char title[32], b[64], b2[64], t[128];
    snprintf(title, sizeof title, "GPU %d", i);
    detail_header(&r, title, g->name);
    Rectangle stats = rect_cut_bottom(&r, 120);
    Rectangle mem = rect_cut_bottom(&r, 150);
    float gw = (r.width - 12) / 2, gh = (r.height - 12) / 2;
    for (int e = 0; e < GPU_ENGINES; ++e) {
        Rectangle gr = { r.x + (e % 2) * (gw + 12), r.y + (e / 2) * (gh + 12), gw, gh };
        Rectangle cap = rect_cut_top(&gr, 22);
        snprintf(b, sizeof b, "%.0f%%", g->eng[e]);
        ui_text_fit((Rectangle){ cap.x, cap.y, cap.width / 2, 20 }, GPU_ENGINE_NAMES[e], 12, FW_REG, T.dim, AL_LEFT);
        ui_text_fit((Rectangle){ cap.x + cap.width / 2, cap.y, cap.width / 2, 20 }, b, 12, FW_SEMI, T.text, AL_RIGHT);
        GraphOpts o = { .maxv = 100, .fmt = GF_PCT, .grid = true, .tooltip = true };
        Series s = { g->h_eng[e].v, 0, TL.latest, T.gpu, GPU_ENGINE_NAMES[e], true, false };
        ui_graph(gr, &s, 1, &o);
    }
    mem.y += 12; mem.height -= 12;
    float mw = (mem.width - 12) / 2;
    Rectangle m1 = { mem.x, mem.y, mw, mem.height }, m2 = { mem.x + mw + 12, mem.y, mw, mem.height };
    fmt_bytes((double)g->ded_total, b, sizeof b);
    graph_caption(&m1, "Dedicated GPU memory usage", b);
    GraphOpts om = { .maxv = (float)(g->ded_total ? g->ded_total : 1), .fmt = GF_BYTES, .grid = true, .tooltip = true };
    Series sm = { g->h_ded.v, 0, TL.latest, T.gpu, "Dedicated", true, false };
    ui_graph(m1, &sm, 1, &om);
    if (g->temp_ok) {
        graph_caption(&m2, "Temperature", "100 \xC2\xB0" "C");
        GraphOpts ot = { .maxv = 100, .fmt = GF_TEMP, .grid = true, .tooltip = true };
        Series stt = { g->h_temp.v, 0, TL.latest, T.warn, "Temperature", true, false };
        ui_graph(m2, &stt, 1, &ot);
    } else {
        fmt_bytes((double)g->shared_total, b, sizeof b);
        graph_caption(&m2, "Shared GPU memory usage", b);
        GraphOpts os = { .maxv = (float)(g->shared_total ? g->shared_total : 1), .fmt = GF_BYTES, .grid = true, .tooltip = true };
        Series ss = { g->h_shared.v, 0, TL.latest, col_mix(T.gpu, T.mem, 0.5f), "Shared", true, false };
        ui_graph(m2, &ss, 1, &os);
    }

    stats.y += 14;
    float cw = (stats.width - 20) / 4;
    snprintf(b, sizeof b, "%.0f%%", g->util);
    stat_item(stats.x + 8, stats.y, cw, "Utilization", b, T.gpu);
    fmt_bytes((double)g->ded_used, b, sizeof b);
    fmt_bytes((double)g->ded_total, b2, sizeof b2);
    snprintf(t, sizeof t, "%s / %s", b, b2);
    stat_item(stats.x + 8 + cw, stats.y, cw, "Dedicated memory", t, BLANK);
    fmt_bytes((double)g->shared_used, b, sizeof b);
    fmt_bytes((double)g->shared_total, b2, sizeof b2);
    snprintf(t, sizeof t, "%s / %s", b, b2);
    stat_item(stats.x + 8 + cw * 2, stats.y, cw, "Shared memory", t, BLANK);
    if (g->temp_ok) {
        snprintf(b, sizeof b, "%.0f \xC2\xB0" "C", g->temp);
        stat_item(stats.x + 8 + cw * 3, stats.y, cw, "Temperature", b, T.warn);
    } else {
        stat_item(stats.x + 8 + cw * 3, stats.y, cw, "Temperature", "\xE2\x80\x94", BLANK);
    }
    window_stats_line((Rectangle){ stats.x, stats.y + 52, stats.width * 0.6f, 22 }, "Utilization", g->h_util.v, GF_PCT, T.gpu);
    if (g->temp_ok)
        window_stats_line((Rectangle){ stats.x, stats.y + 74, stats.width * 0.6f, 22 }, "Temperature", g->h_temp.v, GF_TEMP, T.warn);
    char extra[160];
    snprintf(extra, sizeof extra, "Fan %s  \xC2\xB7  Power %.1f%%  \xC2\xB7  Memory clock %.0f MHz",
             g->fan_rpm ? TextFormat("%u RPM", g->fan_rpm) : "stopped", g->power_pct, g->mem_freq / 1e6);
    ui_text_fit((Rectangle){ stats.x + stats.width * 0.62f, stats.y + 52, stats.width * 0.38f, 22 }, extra, 12, FW_REG, T.dim, AL_RIGHT);
}

void page_performance(Rectangle r)
{
    Dev devs[64];
    int n = devices(devs, 64);
    if (app.perf_dev >= n) app.perf_dev = 0;
    char sub[96];
    snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", app.st->computer, app.st->os_name);
    page_header(&r, "Performance", sub);

    Rectangle list = rect_cut_left(&r, fminf(290, fmaxf(220, r.width * 0.26f)));
    rect_cut_left(&r, 14);
    draw_device_list(list, devs, n);
    card(r);
    Rectangle in = rect_inset(r, 22, 16);
    Dev d = devs[app.perf_dev];
    switch (d.kind) {
    case DEV_CPU:  detail_cpu(in); break;
    case DEV_MEM:  detail_mem(in); break;
    case DEV_DISK: detail_disk(in, d.idx); break;
    case DEV_NET:  detail_net(in, d.idx); break;
    case DEV_GPU:  detail_gpu(in, d.idx); break;
    }

    if (!app.modal && ui.focus == 0) {
        if (IsKeyPressed(KEY_DOWN) && app.perf_dev < n - 1) app.perf_dev++;
        if (IsKeyPressed(KEY_UP) && app.perf_dev > 0) app.perf_dev--;
    }
}
