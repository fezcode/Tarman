/* Hisashi menubar integration (hoswl protocol, see hoswl.h).
 *
 * Tarman publishes conventional menus -- File, Edit, View, Process, Help --
 * rather than an app-named menu. The menu text is rebuilt every frame from
 * the current state (page, selection, toggles) but only sent when its hash
 * changes, so check marks and enabled states track the app without chatter.
 * Clicks come back through hoswl_poll() on the UI thread, inside the frame,
 * so they run exactly like the in-app buttons do. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "hoswl.h"
#include "version.h"

static hoswl_t  g_bar;
static uint32_t g_hash;

static const char* const PAGE_IDS[PAGE_COUNT] = {
    "processes", "performance", "analysis", "network", "disk", "memory",
    "startup", "users", "services", "settings", "firmware", "events",
};
static const char* const PAGE_LABELS[PAGE_COUNT] = {
    "Processes", "Performance", "Analysis", "Network", "Disk", "Memory",
    "Startup apps", "Users", "Services", "Settings", "Firmware", "Events",
};
/* View-menu order and Ctrl+digit hints, matching global_keys() in main.c */
static const int PAGE_ORDER[] = { PAGE_PROCESSES, PAGE_PERFORMANCE, PAGE_ANALYSIS, PAGE_NETWORK, PAGE_DISK,
                                  PAGE_MEMORY, PAGE_STARTUP, PAGE_USERS, PAGE_SERVICES, PAGE_EVENTS, PAGE_FIRMWARE };

typedef struct { char* p; size_t n, cap; } Buf;

static void put(Buf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Buf* b, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w > 0) b->n = b->n + (size_t)w < b->cap ? b->n + (size_t)w : b->cap - 1;
}

/* flags: d disabled, c checkable, x checked */
static const char* fl(bool enabled, int check)
{
    if (!enabled) return check > 0 ? "dx" : check == 0 ? "dc" : "d";
    return check > 0 ? "x" : check == 0 ? "c" : "";
}

static void build(Buf* b)
{
    Proc* p = proc_selected();
    bool sel = p && p->alive;
    bool have_path = p && p->path[0];
    ProcExtra* ex = (p && app.extra_pid == p->pid) ? &app.extra : NULL;

    put(b, "File\n");
    put(b, " file.run|Run new task\xE2\x80\xA6|Ctrl+N\n");
    put(b, " file.admin|Restart as administrator||%s\n", app.st->elevated ? "d" : "");
    put(b, " -\n");
    put(b, " file.settings|Settings\n");
    put(b, " -\n");
    put(b, " file.exit|Exit|Alt+F4\n");

    put(b, "Edit\n");
    put(b, " edit.find|Find\xE2\x80\xA6|Ctrl+F\n");
    put(b, " edit.clear|Clear search|Esc|%s\n", app.search[0] ? "" : "d");
    put(b, " -\n");
    put(b, " edit.copy|Copy|>\n");
    put(b, "  edit.copy.name|Name||%s\n", p ? "" : "d");
    put(b, "  edit.copy.pid|PID||%s\n", p ? "" : "d");
    put(b, "  edit.copy.path|Path||%s\n", have_path ? "" : "d");
    put(b, "  edit.copy.cmd|Command line||%s\n", p && p->cmdline[0] ? "" : "d");

    put(b, "View\n");
    for (size_t i = 0; i < sizeof PAGE_ORDER / sizeof PAGE_ORDER[0]; ++i) {
        int pg = PAGE_ORDER[i];
        if (i < 9) put(b, " view.page.%s|%s|Ctrl+%d|%s\n", PAGE_IDS[pg], PAGE_LABELS[pg], (int)i + 1, fl(true, app.page == pg));
        else put(b, " view.page.%s|%s||%s\n", PAGE_IDS[pg], PAGE_LABELS[pg], fl(true, app.page == pg));
    }
    put(b, " -\n");
    put(b, " view.layout|Process list|>\n");
    put(b, "  view.layout.grouped|Grouped||%s\n", fl(true, app.proc_view == 0));
    put(b, "  view.layout.tree|Tree||%s\n", fl(true, app.proc_view == 1));
    put(b, "  view.layout.flat|Flat||%s\n", fl(true, app.proc_view == 2));
    put(b, " view.details|Details panel|Enter|%s\n", fl(true, app.details_open));
    put(b, " -\n");
    put(b, " view.window|Time window|>\n");
    put(b, "  view.window.1|1 minute||%s\n", fl(true, app.window_idx == 0));
    put(b, "  view.window.5|5 minutes||%s\n", fl(true, app.window_idx == 1));
    put(b, "  view.window.10|10 minutes||%s\n", fl(true, app.window_idx == 2));
    put(b, " view.pause|Pause recording|Space|%s\n", fl(true, app.paused));
    put(b, " -\n");
    put(b, " view.theme|Theme|>\n");
    put(b, "  view.theme.dark|Dark||%s\n", fl(true, app.dark));
    put(b, "  view.theme.light|Light||%s\n", fl(true, !app.dark));
    put(b, " view.zoomin|Zoom in|Ctrl+Plus\n");
    put(b, " view.zoomout|Zoom out|Ctrl+Minus\n");
    put(b, " view.zoomreset|Actual size|Ctrl+0\n");
    put(b, " -\n");
    put(b, " view.sidebar|Compact sidebar||%s\n", fl(true, app.nav_collapsed));
    put(b, " view.ontop|Always on top||%s\n", fl(true, app.topmost));

    put(b, "Process\n");
    put(b, " proc.end|End task|Del|%s\n", sel ? "" : "d");
    put(b, " proc.tree|End process tree||%s\n", sel ? "" : "d");
    put(b, " -\n");
    if (p && p->is_suspended) put(b, " proc.resume|Resume||%s\n", sel ? "" : "d");
    else put(b, " proc.suspend|Suspend||%s\n", sel ? "" : "d");
    put(b, " proc.eff|Efficiency mode||%s\n", fl(sel && ex && ex->ok, ex ? ex->efficiency : 0));
    put(b, " proc.prio|Set priority|>\n");
    for (int i = 0; i < 6; ++i)
        put(b, "  proc.prio.%d|%s||%s\n", i, PRIO_CLASSES[i].name,
            fl(sel, ex && ex->prio_class == PRIO_CLASSES[i].cls ? 1 : 0));
    put(b, " proc.affinity|Set affinity\xE2\x80\xA6||%s\n", sel ? "" : "d");
    put(b, " -\n");
    put(b, " proc.dump|Create memory dump file||%s\n", sel ? "" : "d");
    put(b, " proc.location|Open file location||%s\n", have_path ? "" : "d");
    put(b, " proc.search|Search online||%s\n", p ? "" : "d");
    put(b, " proc.props|Properties||%s\n", have_path ? "" : "d");

    put(b, "Help\n");
    put(b, " help.shortcuts|Keyboard shortcuts|F1\n");
    put(b, " help.about|About Tarman\n");
}

static uint32_t fnv(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

void menubar_init(bool enabled)
{
    hoswl_init(&g_bar, "com.fezcode.tarman", "Tarman", TARMAN_VERSION);
    hoswl_set_enabled(&g_bar, enabled ? 1 : 0);
}

void menubar_set_enabled(bool on)
{
    if (g_bar.inited) hoswl_set_enabled(&g_bar, on ? 1 : 0);
}

bool menubar_connected(void)
{
    return g_bar.inited && hoswl_connected(&g_bar);
}

void menubar_shutdown(void)
{
    if (g_bar.inited) hoswl_shutdown(&g_bar);
}

static void sel_action(int action)
{
    Proc* p = proc_selected();
    if (p) proc_action(action, p->pid, p->create_time);
}

static void dispatch(const char* id)
{
    if (!strcmp(id, "file.run")) open_run_dialog();
    else if (!strcmp(id, "file.admin")) { if (sys_restart_elevated()) app.quit = true; }
    else if (!strcmp(id, "file.settings")) app.page = PAGE_SETTINGS;
    else if (!strcmp(id, "file.exit")) app.quit = true;
    else if (!strcmp(id, "edit.find")) { app.page = app.page == PAGE_PROCESSES ? app.page : PAGE_PROCESSES; ui.focus_next = ui_id("proc-search", 0); }
    else if (!strcmp(id, "edit.clear")) app.search[0] = 0;
    else if (!strcmp(id, "edit.copy.name")) sel_action(PA_COPY_NAME);
    else if (!strcmp(id, "edit.copy.pid")) sel_action(PA_COPY_PID);
    else if (!strcmp(id, "edit.copy.path")) sel_action(PA_COPY_PATH);
    else if (!strcmp(id, "edit.copy.cmd")) sel_action(PA_COPY_CMD);
    else if (!strncmp(id, "view.page.", 10)) {
        for (int i = 0; i < PAGE_COUNT; ++i) if (!strcmp(id + 10, PAGE_IDS[i])) app.page = i;
    }
    else if (!strcmp(id, "view.layout.grouped")) app.proc_view = 0;
    else if (!strcmp(id, "view.layout.tree")) app.proc_view = 1;
    else if (!strcmp(id, "view.layout.flat")) app.proc_view = 2;
    else if (!strcmp(id, "view.details")) app.details_open = !app.details_open;
    else if (!strcmp(id, "view.window.1")) app.window_idx = 0;
    else if (!strcmp(id, "view.window.5")) app.window_idx = 1;
    else if (!strcmp(id, "view.window.10")) app.window_idx = 2;
    else if (!strcmp(id, "view.pause")) { app.paused = !app.paused; sys_set_paused(app.paused); }
    else if (!strcmp(id, "view.theme.dark")) { app.dark = true; ui_theme(true); }
    else if (!strcmp(id, "view.theme.light")) { app.dark = false; ui_theme(false); }
    else if (!strcmp(id, "view.zoomin")) app.zoom = app.zoom + 0.1f > 2.0f ? 2.0f : app.zoom + 0.1f;
    else if (!strcmp(id, "view.zoomout")) app.zoom = app.zoom - 0.1f < 0.8f ? 0.8f : app.zoom - 0.1f;
    else if (!strcmp(id, "view.zoomreset")) app.zoom = 1.0f;
    else if (!strcmp(id, "view.sidebar")) app.nav_collapsed = !app.nav_collapsed;
    else if (!strcmp(id, "view.ontop")) app.topmost = !app.topmost;
    else if (!strcmp(id, "proc.end")) sel_action(PA_END);
    else if (!strcmp(id, "proc.tree")) sel_action(PA_END_TREE);
    else if (!strcmp(id, "proc.suspend")) sel_action(PA_SUSPEND);
    else if (!strcmp(id, "proc.resume")) sel_action(PA_RESUME);
    else if (!strcmp(id, "proc.eff")) sel_action(PA_EFFICIENCY);
    else if (!strncmp(id, "proc.prio.", 10)) sel_action(PA_PRIO_BASE + (id[10] - '0'));
    else if (!strcmp(id, "proc.affinity")) sel_action(PA_AFFINITY);
    else if (!strcmp(id, "proc.dump")) sel_action(PA_DUMP);
    else if (!strcmp(id, "proc.location")) sel_action(PA_LOCATION);
    else if (!strcmp(id, "proc.search")) sel_action(PA_SEARCH);
    else if (!strcmp(id, "proc.props")) sel_action(PA_PROPERTIES);
    else if (!strcmp(id, "help.shortcuts") || !strcmp(id, "help.about")) app.modal = MODAL_ABOUT;
    settings_save();
}

void menubar_frame(void)
{
    if (!g_bar.inited) return;
    static char text[16384];
    Buf b = { text, 0, sizeof text };
    text[0] = 0;
    build(&b);
    uint32_t h = fnv(text, b.n);
    if (h != g_hash) {
        if (hoswl_set_menus(&g_bar, text) == 0) g_hash = h;
        else { g_hash = h; toast(true, "Hisashi menubar: %s", g_bar.last_error); }
    }
    const char* id;
    while ((id = hoswl_poll(&g_bar)) != NULL) {
        char copy[HOSWL_ID_MAX];
        snprintf(copy, sizeof copy, "%s", id);
        dispatch(copy);
    }
}
