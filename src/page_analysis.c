/* Analysis page: the ten-minute moving window as one timeline.
 *
 * Top: CPU, memory, disk, network and GPU stacked on a shared time axis,
 * with process start/exit ticks. Hover scrubs every lane at once; click pins
 * a moment. Right: who was on top at that moment (every per-process ring is
 * aligned to the same sample clock, so "at moment k" is one array read).
 * Bottom: per-process aggregates over the window -- average and peak CPU,
 * CPU-seconds, memory peak and growth (a leak smell), total I/O -- including
 * processes that have already exited, which is usually the interesting part. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static Hist g_disk_total, g_net_total, g_gpu_max;

/* Aggregate lanes recomputed per frame for the visible window. */
static void build_lanes(void)
{
    SysState* st = app.st;
    uint64_t a = tl_left();
    for (uint64_t k = a; TL.count && k <= TL.latest; ++k) {
        int i = (int)(k % HIST_LEN);
        float d = 0, n = 0, g = 0;
        for (int j = 0; j < st->ndisk; ++j) d += st->disk[j].h_read.v[i] + st->disk[j].h_write.v[i];
        for (int j = 0; j < st->nnet; ++j) n += st->net[j].h_recv.v[i] + st->net[j].h_send.v[i];
        for (int j = 0; j < st->ngpu; ++j) g = fmaxf(g, st->gpu[j].h_util.v[i]);
        g_disk_total.v[i] = d;
        g_net_total.v[i] = n;
        g_gpu_max.v[i] = g;
    }
}

typedef struct {
    Proc*  p;
    float  avg_cpu, peak_cpu, cpu_sec;
    float  peak_mem, mem_growth, avg_mem;
    double io_bytes;
    float  avg_gpu;
    int    samples;
} Agg;

enum { A_NAME, A_PID, A_AVG, A_PEAK, A_CPUSEC, A_MEMPEAK, A_GROWTH, A_IO, A_GPU, A_LIFE, A_COUNT };
static const Column ACOLS[A_COUNT] = {
    { "Process",          0,   AL_LEFT,  0 },
    { "PID",              64,  AL_RIGHT, 5 },
    { "Avg CPU",          88,  AL_RIGHT, 0 },
    { "Peak CPU",         92,  AL_RIGHT, 0 },
    { "CPU time",         88,  AL_RIGHT, 4 },
    { "Peak memory",      100, AL_RIGHT, 0 },
    { "Memory \xCE\x94",  96,  AL_RIGHT, 0 },
    { "I/O total",        92,  AL_RIGHT, 3 },
    { "Avg GPU",          74,  AL_RIGHT, 6 },
    { "In window",        132, AL_LEFT,  2 },
};

static Agg  g_agg[8192];
static int  g_nagg;
static int  s_sort;
static bool s_desc;

#define CMPNUM(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)

static int agg_cmp(const void* A, const void* B)
{
    const Agg* a = A;
    const Agg* b = B;
    int r = 0;
    switch (s_sort) {
    case A_NAME:    r = strcmp(a->p->name, b->p->name); break;
    case A_PID:     r = CMPNUM(a->p->pid, b->p->pid); break;
    case A_AVG:     r = CMPNUM(a->avg_cpu, b->avg_cpu); break;
    case A_PEAK:    r = CMPNUM(a->peak_cpu, b->peak_cpu); break;
    case A_CPUSEC:  r = CMPNUM(a->cpu_sec, b->cpu_sec); break;
    case A_MEMPEAK: r = CMPNUM(a->peak_mem, b->peak_mem); break;
    case A_GROWTH:  r = CMPNUM(a->mem_growth, b->mem_growth); break;
    case A_IO:      r = CMPNUM(a->io_bytes, b->io_bytes); break;
    case A_GPU:     r = CMPNUM(a->avg_gpu, b->avg_gpu); break;
    case A_LIFE:    r = CMPNUM(a->samples, b->samples); break;
    }
    if (s_desc) r = -r;
    if (!r) r = CMPNUM(a->p->pid, b->p->pid);
    return r;
}

static void build_agg(void)
{
    SysState* st = app.st;
    uint64_t left = tl_left();
    int ncpu = st->cpu.logical > 0 ? st->cpu.logical : 1;
    g_nagg = 0;
    for (int i = 0; i < st->nproc && g_nagg < 8192; ++i) {
        Proc* p = st->procs[i];
        if (!app.an_show_exited && !p->alive) continue;
        if (app.search[0] && !text_match(p->name, app.search) && !text_match(p->desc, app.search)) continue;
        uint64_t a = p->first_sample > left ? p->first_sample : left;
        uint64_t b = p->last_sample < TL.latest ? p->last_sample : TL.latest;
        if (!TL.count || a > b) continue;
        Agg* g = &g_agg[g_nagg++];
        memset(g, 0, sizeof *g);
        g->p = p;
        double sc = 0, sm = 0, sg = 0;
        for (uint64_t k = a; k <= b; ++k) {
            int j = (int)(k % HIST_LEN);
            float c = p->h_cpu[j];
            sc += c;
            if (c > g->peak_cpu) g->peak_cpu = c;
            sm += p->h_mem[j];
            if (p->h_mem[j] > g->peak_mem) g->peak_mem = p->h_mem[j];
            g->io_bytes += p->h_io[j];
            sg += p->h_gpu[j];
            g->samples++;
        }
        g->avg_cpu = (float)(sc / g->samples);
        g->cpu_sec = (float)(sc / 100.0 * ncpu);  /* % of machine-seconds -> core-seconds */
        g->avg_mem = (float)(sm / g->samples);
        g->mem_growth = p->h_mem[b % HIST_LEN] - p->h_mem[a % HIST_LEN];
        g->avg_gpu = (float)(sg / g->samples);
    }
    s_sort = app.an_sort;
    s_desc = app.an_desc;
    qsort(g_agg, (size_t)g_nagg, sizeof g_agg[0], agg_cmp);
}

/* ---- timeline lanes --------------------------------------------------------------------- */

static void lane(Rectangle r, const char* name, const char* now, const Series* s, int ns, float maxv, int fmt)
{
    Rectangle label = rect_cut_left(&r, 128);
    ui_text_fit((Rectangle){ label.x, label.y + label.height / 2 - 20, label.width - 10, 20 }, name, 12.5f, FW_SEMI, T.dim, AL_LEFT);
    ui_text_fit((Rectangle){ label.x, label.y + label.height / 2, label.width - 10, 22 }, now, 15, FW_SEMI, s[0].color, AL_LEFT);
    GraphOpts o = { .maxv = maxv, .minmax = fmt == GF_PCT ? 1 : 1024, .fmt = fmt, .grid = true, .axis_labels = true, .tooltip = true };
    ui_graph(r, s, ns, &o);
}

static void event_ticks(Rectangle r)
{
    /* process starts (green) and exits (red) inside the window, under the lanes */
    SysState* st = app.st;
    uint64_t left = tl_left();
    float span = (float)(TL.window - 1);
    for (int i = 0; i < st->nproc; ++i) {
        Proc* p = st->procs[i];
        if (p->first_sample > left && p->first_sample <= TL.latest) {
            float x = r.x + r.width * (float)((double)p->first_sample - ((double)TL.latest - span)) / span;
            DrawRectangleRec((Rectangle){ x - 0.75f, r.y + 2, 1.5f, r.height / 2 - 3 }, col_alpha(T.good, 0.8f));
        }
        if (!p->alive && p->last_sample >= left) {
            float x = r.x + r.width * (float)((double)p->last_sample + 1 - ((double)TL.latest - span)) / span;
            DrawRectangleRec((Rectangle){ x - 0.75f, r.y + r.height / 2 + 1, 1.5f, r.height / 2 - 3 }, col_alpha(T.bad, 0.8f));
        }
    }
}

static void time_axis(Rectangle r)
{
    /* labels every minute (10 s on the 1-minute window), from real sample times */
    if (!TL.count) return;
    float span = (float)(TL.window - 1);
    int step = TL.window <= 60 ? 10 : 60;
    for (uint64_t k = tl_left(); k <= TL.latest; ++k) {
        int64_t t = TL.times[k % HIST_LEN] / 1000;
        if (t % step) continue;
        float x = r.x + r.width * (float)((double)k - ((double)TL.latest - span)) / span;
        char c[16], ago[24], lab[48];
        sys_format_clock(TL.times[k % HIST_LEN], c, sizeof c);
        fmt_dur((double)(TL.latest - k), ago, sizeof ago);
        snprintf(lab, sizeof lab, step < 60 ? "%s" : "%.5s", c);   /* seconds matter on the 1-minute view */
        (void)ago;
        float w = ui_text_w(lab, 11, FW_REG);
        if (x - w / 2 < r.x || x + w / 2 > r.x + r.width) continue;
        ui_text(x - w / 2, r.y + 2, lab, 11, FW_REG, T.faint);
    }
}

/* ---- "at this moment" panel -------------------------------------------------------------- */

typedef struct { Proc* p; float v; } Top;

static int top_cmp(const void* A, const void* B)
{
    float a = ((const Top*)A)->v, b = ((const Top*)B)->v;
    return a < b ? 1 : a > b ? -1 : 0;
}

static void top_list(Rectangle* r, const char* title, uint64_t k, int which, Color c, int max_rows)
{
    SysState* st = app.st;
    static Top tops[8192];
    int n = 0;
    for (int i = 0; i < st->nproc && n < 8192; ++i) {
        Proc* p = st->procs[i];
        if (k < p->first_sample || k > p->last_sample) continue;
        int j = (int)(k % HIST_LEN);
        float v = which == 0 ? p->h_cpu[j] : which == 1 ? p->h_mem[j] : which == 2 ? p->h_io[j] : p->h_gpu[j];
        if (v <= 0) continue;
        tops[n++] = (Top){ p, v };
    }
    qsort(tops, (size_t)n, sizeof tops[0], top_cmp);
    Rectangle h = rect_cut_top(r, 24);
    ui_text_fit(h, title, 12, FW_SEMI, T.dim, AL_LEFT);
    float maxv = n ? tops[0].v : 1;
    for (int i = 0; i < n && i < max_rows; ++i) {
        Rectangle row = rect_cut_top(r, 26);
        Proc* p = tops[i].p;
        bool hov = ui_hover(row);
        if (hov) { ui_rrect(row, 5, T.hover); ui.cursor = MOUSE_CURSOR_POINTING_HAND; }
        float frac = tops[i].v / maxv;
        ui_rrect((Rectangle){ row.x + 2, row.y + 3, (row.width - 4) * frac, row.height - 6 }, 4, col_alpha(c, 0.13f));
        proc_icon(p, row.x + 14, row.y + 13, 15);
        char v[32];
        if (which == 0 || which == 3) snprintf(v, sizeof v, "%.1f%%", tops[i].v);
        else if (which == 1) fmt_bytes(tops[i].v, v, sizeof v);
        else fmt_rate(tops[i].v, v, sizeof v);
        float vw = 76;
        ui_text_fit((Rectangle){ row.x + 28, row.y, row.width - 32 - vw, row.height }, proc_display_name(p), 12.5f, FW_REG,
                    p->alive ? T.text : T.faint, AL_LEFT);
        ui_text_fit((Rectangle){ row.x + row.width - vw - 6, row.y, vw, row.height }, v, 12.5f, FW_SEMI, T.text, AL_RIGHT);
        if (hov && ui_clicked(row)) { proc_select(p); }
        if (ui_rclicked(row)) proc_menu_open(p, ui.mouse);
        if (hov) {
            char tip[200];
            snprintf(tip, sizeof tip, "%s (PID %u)%s\nRight-click for actions", p->name, p->pid, p->alive ? "" : "\nexited");
            ui_tooltip(tip);
        }
    }
    if (!n) {
        Rectangle row = rect_cut_top(r, 26);
        ui_text_fit(row, "Nothing notable", 12, FW_REG, T.faint, AL_LEFT);
    }
    rect_cut_top(r, 10);
}

static void moment_panel(Rectangle r)
{
    card(r);
    Rectangle in = rect_inset(r, 16, 14);
    int64_t k = TL.pin >= 0 ? TL.pin : (TL.hover >= 0 ? TL.hover : (int64_t)TL.latest);
    if (!TL.count) return;
    if (k < (int64_t)tl_left()) k = (int64_t)tl_left();
    char clock[16], ago[24], head[96];
    sys_format_clock(TL.times[k % HIST_LEN], clock, sizeof clock);
    fmt_dur((double)(TL.latest - (uint64_t)k), ago, sizeof ago);
    Rectangle h = rect_cut_top(&in, 46);
    const char* mode = TL.pin >= 0 ? "Pinned moment" : TL.hover >= 0 ? "Cursor" : "Now";
    ui_text_fit((Rectangle){ h.x, h.y, h.width, 18 }, mode, 12, FW_SEMI, TL.pin >= 0 ? T.warn : T.dim, AL_LEFT);
    snprintf(head, sizeof head, "%s  ", clock);
    ui_text_fit((Rectangle){ h.x, h.y + 18, h.width, 26 }, head, 18, FW_SEMI, T.text, AL_LEFT);
    if ((uint64_t)k != TL.latest) {
        snprintf(head, sizeof head, "%s ago", ago);
        ui_text_fit((Rectangle){ h.x + 100, h.y + 22, h.width - 100, 22 }, head, 12.5f, FW_REG, T.dim, AL_LEFT);
    }
    if (TL.pin >= 0) {
        Rectangle b = { h.x + h.width - 70, h.y + 4, 70, 28 };
        if (ui_button(b, "Unpin", IC_PIN, BTN_SUBTLE, true)) TL.pin = -1;
    }
    /* share the height between the lists instead of running off the card */
    int nl = app.st->ngpu ? 4 : 3;
    int rows = (int)((in.height - nl * 34) / 26 / nl);
    if (rows > 5) rows = 5;
    if (rows < 1) { nl = 2; rows = 1; }
    ui_scissor_push(in);
    top_list(&in, "Top CPU", (uint64_t)k, 0, T.cpu, rows);
    top_list(&in, "Top memory", (uint64_t)k, 1, T.mem, rows);
    if (nl > 2) top_list(&in, "Top disk I/O", (uint64_t)k, 2, T.disk, rows);
    if (nl > 3) top_list(&in, "Top GPU", (uint64_t)k, 3, T.gpu, rows);
    ui_scissor_pop();
}

/* ---- window summary table ----------------------------------------------------------------- */

static void summary_table(Rectangle r)
{
    card(r);
    Rectangle in = rect_inset(r, 1, 1);
    Rectangle title = rect_cut_top(&in, 44);
    char t[96];
    snprintf(t, sizeof t, "Window summary  \xC2\xB7  %d processes", g_nagg);
    ui_text_fit((Rectangle){ title.x + 14, title.y + 6, 360, 32 }, t, 14, FW_SEMI, T.text, AL_LEFT);
    Rectangle cb = { title.x + title.width - 190, title.y + 8, 180, 28 };
    ui_checkbox(cb, "Include exited", &app.an_show_exited);

    ColLayout L;
    table_layout(ACOLS, A_COUNT, (Rectangle){ in.x + 4, in.y, in.width - 16, in.height }, &L, 180);
    Rectangle head = rect_cut_top(&in, 32);
    if (table_header(head, ACOLS, &L, &app.an_sort, &app.an_desc)) app.an_scroll = 0;
    const float RH = 30;
    Rectangle body = rect_inset(in, 0, 2);
    int first = table_scroll(body, &app.an_scroll, g_nagg, RH, ui_id("an-scroll", 0));
    ui_scissor_push(body);
    uint64_t left = tl_left();
    for (int i = first; i < g_nagg; ++i) {
        float y = body.y + i * RH - app.an_scroll;
        if (y > body.y + body.height) break;
        Agg* g = &g_agg[i];
        Proc* p = g->p;
        Rectangle rr = { body.x + 4, y, body.width - 16, RH };
        bool sel = p->pid == app.sel_pid && p->create_time == app.sel_create;
        bool hov = ui_hover(rr) && CheckCollisionPointRec(ui.mouse, body);
        if (sel) ui_rrect(rr, 6, T.sel);
        else if (hov) ui_rrect(rr, 6, T.hover);
        Color tc = p->alive ? T.text : T.faint;
        Rectangle nc = table_cell_rect(rr, &L, A_NAME);
        proc_icon(p, nc.x + 9, rr.y + RH / 2, 16);
        ui_text_fit((Rectangle){ nc.x + 24, rr.y, nc.width - 24, RH }, proc_display_name(p), 13, FW_REG, tc, AL_LEFT);
        char b[64];
        snprintf(b, sizeof b, "%u", p->pid);
        table_cell(rr, &L, A_PID, b, T.dim, FW_REG, AL_RIGHT);
        snprintf(b, sizeof b, "%.1f%%", g->avg_cpu);
        table_cell(rr, &L, A_AVG, b, tc, FW_REG, AL_RIGHT);
        snprintf(b, sizeof b, "%.1f%%", g->peak_cpu);
        table_cell(rr, &L, A_PEAK, b, g->peak_cpu > 50 ? T.warn : tc, FW_REG, AL_RIGHT);
        fmt_dur(g->cpu_sec, b, sizeof b);
        if (g->cpu_sec < 60) snprintf(b, sizeof b, "%.1f s", g->cpu_sec);
        table_cell(rr, &L, A_CPUSEC, b, tc, FW_REG, AL_RIGHT);
        fmt_bytes(g->peak_mem, b, sizeof b);
        table_cell(rr, &L, A_MEMPEAK, b, tc, FW_REG, AL_RIGHT);
        char gb[32];
        fmt_bytes(fabsf(g->mem_growth), gb, sizeof gb);
        snprintf(b, sizeof b, "%s%s", g->mem_growth > 0 ? "+" : g->mem_growth < 0 ? "\xE2\x88\x92" : "", gb);
        Color gc = g->mem_growth > 50e6f ? T.warn : g->mem_growth > 0 ? tc : T.dim;
        table_cell(rr, &L, A_GROWTH, b, gc, FW_REG, AL_RIGHT);
        fmt_bytes(g->io_bytes, b, sizeof b);
        table_cell(rr, &L, A_IO, b, tc, FW_REG, AL_RIGHT);
        snprintf(b, sizeof b, "%.1f%%", g->avg_gpu);
        table_cell(rr, &L, A_GPU, b, tc, FW_REG, AL_RIGHT);
        if (L.vis[A_LIFE]) {
            /* a mini lifeline: where in the window this process existed */
            Rectangle lc = table_cell_rect(rr, &L, A_LIFE);
            Rectangle bar = { lc.x, rr.y + RH / 2 - 3, lc.width, 6 };
            ui_rrect(bar, 3, col_alpha(T.faint, 0.2f));
            float span = (float)TL.window;
            uint64_t a = p->first_sample > left ? p->first_sample : left;
            uint64_t bk = p->last_sample < TL.latest ? p->last_sample : TL.latest;
            float x0 = (float)(a - left) / span, x1 = (float)(bk - left + 1) / span;
            if (TL.latest + 1 < (uint64_t)TL.window) {   /* window not yet full */
                x0 = (float)(a - left) / (float)(TL.latest - left + 1);
                x1 = (float)(bk - left + 1) / (float)(TL.latest - left + 1);
            }
            ui_rrect((Rectangle){ bar.x + bar.width * x0, bar.y, fmaxf(3, bar.width * (x1 - x0)), bar.height }, 3,
                     p->alive ? col_alpha(T.accent, 0.8f) : col_alpha(T.bad, 0.7f));
        }
        if (hov) {
            if (ui_rclicked(rr)) { proc_select(p); proc_menu_open(p, ui.mouse); }
            else if (ui_clicked(rr)) proc_select(p);
            if (!p->alive) ui_tooltip("This process exited during the window");
        }
    }
    ui_scissor_pop();
    if (!g_nagg) empty_state(body, IC_HISTORY, "Collecting samples\xE2\x80\xA6");
}

void page_analysis(Rectangle r)
{
    SysState* st = app.st;
    char sub[128];
    snprintf(sub, sizeof sub, "%llu samples recorded  \xC2\xB7  hover to scrub, click to pin",
             (unsigned long long)TL.count);
    Rectangle tb = page_header(&r, "Analysis", sub);
    Rectangle sr = rect_cut_right(&tb, fminf(240, tb.width));
    ui_textbox(sr, app.search, sizeof app.search, ui_id("an-search", 0), "Filter processes", IC_SEARCH);

    build_lanes();
    build_agg();

    Rectangle top = rect_cut_top(&r, fmaxf(300, r.height * 0.55f));
    rect_cut_top(&r, 12);
    Rectangle side = rect_cut_right(&top, fminf(340, top.width * 0.32f));
    rect_cut_right(&top, 12);

    card(top);
    Rectangle lanes = rect_inset(top, 14, 12);
    Rectangle axis = rect_cut_bottom(&lanes, 18);
    Rectangle ev = rect_cut_bottom(&lanes, 16);
    int nl = 4 + (st->ngpu ? 1 : 0);
    float lh = (lanes.height - (nl - 1) * 6) / nl;
    char now[48];

    Rectangle l = rect_cut_top(&lanes, lh);
    snprintf(now, sizeof now, "%.0f%%", st->cpu.usage);
    Series sc[2] = { { st->cpu.h_usage.v, 0, TL.latest, T.cpu, "CPU", true, false },
                     { st->cpu.h_kernel.v, 0, TL.latest, T.kernel, "Kernel", false, false } };
    lane(l, "CPU", now, sc, 2, 100, GF_PCT);
    rect_cut_top(&lanes, 6);

    l = rect_cut_top(&lanes, lh);
    fmt_bytes((double)st->mem.used, now, sizeof now);
    Series sm[2] = { { st->mem.h_used.v, 0, TL.latest, T.mem, "In use", true, false },
                     { st->mem.h_commit.v, 0, TL.latest, col_mix(T.mem, T.cpu, 0.4f), "Committed", false, false } };
    lane(l, "Memory", now, sm, 2, (float)(st->mem.commit_limit > st->mem.total ? st->mem.commit_limit : st->mem.total), GF_BYTES);
    rect_cut_top(&lanes, 6);

    l = rect_cut_top(&lanes, lh);
    double d = 0;
    for (int i = 0; i < st->ndisk; ++i) d += st->disk[i].read_bps + st->disk[i].write_bps;
    fmt_rate(d, now, sizeof now);
    Series sd = { g_disk_total.v, 0, TL.latest, T.disk, "Disk", true, false };
    lane(l, "Disk", now, &sd, 1, 0, GF_RATE);
    rect_cut_top(&lanes, 6);

    l = rect_cut_top(&lanes, lh);
    double n = 0;
    for (int i = 0; i < st->nnet; ++i) n += st->net[i].recv_bps + st->net[i].send_bps;
    fmt_bits(n, now, sizeof now);
    Series sn = { g_net_total.v, 0, TL.latest, T.net, "Network", true, false };
    lane(l, "Network", now, &sn, 1, 0, GF_BITS);

    if (st->ngpu) {
        rect_cut_top(&lanes, 6);
        l = rect_cut_top(&lanes, lh);
        float g = 0;
        for (int i = 0; i < st->ngpu; ++i) g = fmaxf(g, st->gpu[i].util);
        snprintf(now, sizeof now, "%.0f%%", g);
        Series sg = { g_gpu_max.v, 0, TL.latest, T.gpu, "GPU", true, false };
        lane(l, "GPU", now, &sg, 1, 100, GF_PCT);
    }
    Rectangle evr = { ev.x + 128, ev.y, ev.width - 128, ev.height };
    ui_text_fit((Rectangle){ ev.x, ev.y, 128, ev.height }, "Starts / exits", 11, FW_REG, T.faint, AL_LEFT);
    ui_rrect(evr, 3, col_alpha(T.faint, 0.08f));
    event_ticks(evr);
    time_axis((Rectangle){ axis.x + 128, axis.y, axis.width - 128, axis.height });

    moment_panel(side);
    summary_table(r);
}
