#ifndef TARMAN_APP_H
#define TARMAN_APP_H

/* Application state shared by main.c and the page_*.c files. Everything a
 * page draws is read from SysState under sys_lock(), which main.c holds for
 * the whole frame; pages must never keep a Proc* across frames (the sampler
 * frees exited processes) -- selections are stored as (pid, create_time). */

#include "sys.h"
#include "ui.h"

typedef enum {
    PAGE_PROCESSES = 0, PAGE_PERFORMANCE, PAGE_ANALYSIS, PAGE_NETWORK, PAGE_DISK, PAGE_MEMORY,
    PAGE_STARTUP, PAGE_USERS, PAGE_SERVICES, PAGE_SETTINGS, PAGE_FIRMWARE, PAGE_EVENTS, PAGE_COUNT
} Page;

typedef enum {
    MODAL_NONE = 0, MODAL_CONFIRM, MODAL_RUN, MODAL_AFFINITY, MODAL_ABOUT
} ModalKind;

/* Process actions, shared by every context menu that targets a process. */
enum {
    PA_END = 1, PA_END_TREE, PA_SUSPEND, PA_RESUME, PA_EFFICIENCY, PA_AFFINITY, PA_DUMP,
    PA_LOCATION, PA_SEARCH, PA_PROPERTIES, PA_COPY_NAME, PA_COPY_PID, PA_COPY_PATH,
    PA_COPY_CMD, PA_DETAILS, PA_GOTO_PROCESS,
    PA_PRIO_BASE = 100,        /* + index into PRIO_CLASSES */
    PA_SVC_START = 200, PA_SVC_STOP, PA_SVC_RESTART, PA_SVC_OPEN,
    PA_ST_ENABLE = 300, PA_ST_DISABLE, PA_ST_LOCATION, PA_ST_SEARCH, PA_ST_PROPS,
    PA_SES_DISCONNECT = 400, PA_SES_SIGNOFF,
};

enum { MENU_OWNER_PROC = 1, MENU_OWNER_SERVICE, MENU_OWNER_STARTUP, MENU_OWNER_SESSION };

typedef struct { const char* name; uint32_t cls; int base; } PrioClass;
extern const PrioClass PRIO_CLASSES[6];

typedef struct {
    char   text[256];
    double until;
    bool   error;
} Toast;

typedef struct {
    SysState* st;
    int       page;
    bool      nav_collapsed;
    bool      dark, topmost;
    float     zoom;               /* user scale on top of the monitor DPI */
    int       window_idx;         /* 0: 1 min, 1: 5 min, 2: 10 min */
    bool      paused;
    bool      quit;
    bool      hoswl;              /* publish menus to Hisashi's menubar */

    /* processes */
    char      search[128];
    int       proc_view;          /* 0 grouped, 1 tree, 2 flat */
    int       proc_sort;
    bool      proc_desc;
    float     proc_scroll;
    uint32_t  sel_pid;
    uint64_t  sel_create;
    bool      group_collapsed[PGROUP_COUNT];
    bool      details_open;
    int       details_tab;        /* 0 overview, 1 modules */
    ProcExtra extra;
    double    extra_time;
    uint32_t  extra_pid;
    ModuleInfo* mods;
    int       nmods;
    uint32_t  mods_pid;
    uint64_t  mods_create;
    float     mods_scroll;
    float     details_scroll;
    uint32_t  tree_collapsed[256];
    int       ntree_collapsed;
    bool      scroll_to_sel;

    /* performance */
    int       perf_dev;
    bool      perf_cores;
    float     perf_list_scroll;

    /* analysis */
    int       an_sort;
    bool      an_desc;
    float     an_scroll;
    float     an_events_scroll;
    bool      an_show_exited;

    /* resource pages */
    int       net_tab;
    float     net_scroll;
    int       conn_sort;
    bool      conn_desc;
    float     disk_scroll;
    int       disk_sort;
    bool      disk_desc;
    float     vol_scroll;
    float     mem_scroll;
    int       mem_sort;
    bool      mem_desc;

    /* system pages */
    int       st_sel;
    float     st_scroll;
    int       st_sort;
    bool      st_desc;
    int       svc_sort;
    bool      svc_desc;
    float     svc_scroll;
    char      svc_sel[96];
    char      svc_search[96];
    uint32_t  user_sel;
    float     user_scroll;

    /* overlays */
    Menu      menu;
    int       modal;
    char      modal_title[96];
    char      modal_text[320];
    int       modal_action;       /* PA_* to run on confirm */
    uint32_t  modal_pid;
    uint64_t  modal_create;
    char      modal_name[96];
    char      run_cmd[512];
    bool      run_admin;
    uint64_t  aff_mask, aff_sys;

    /* events */
    char      ev_source[160];     /* "" = Tarman activity, else a channel path */
    int       ev_level, ev_time;
    char      ev_search[128];
    float     ev_scroll, ev_list_scroll, ev_detail_scroll;
    int       ev_sel;
    bool      ev_xml, ev_show_proc;

    FirmwareInfo fw;
    bool      fw_loaded;

    Toast     toasts[4];
    int       ntoasts;

    Texture2D* icon_tex;
    int        nicon_tex, icon_tex_cap;
} App;

extern App app;

/* ---- common.c --------------------------------------------------------------- */

void   toast(bool error, const char* fmt, ...);
void   toasts_draw(void);
Proc*  proc_find(uint32_t pid, uint64_t create);
Proc*  proc_find_alive(uint32_t pid);
Proc*  proc_selected(void);
void   proc_select(const Proc* p);
void   proc_icon(const Proc* p, float cx, float cy, float size);
void   icons_upload(void);
void   icons_free(void);
const char* proc_display_name(const Proc* p);
const char* prio_name(int32_t base_prio);
void   proc_menu_open(const Proc* p, Vector2 at);
void   proc_action(int action, uint32_t pid, uint64_t create);
void   menu_dispatch(int id);
bool   text_match(const char* hay, const char* needle);   /* case-insensitive */

/* Sortable table columns. */
typedef struct {
    const char* name;
    float       w;          /* fixed width; 0 = flex */
    int         align;
    int         prio;       /* hide columns with higher prio first when narrow */
} Column;

#define MAX_COLS 16
typedef struct {
    float x[MAX_COLS], w[MAX_COLS];
    bool  vis[MAX_COLS];
    int   n;
} ColLayout;

void table_layout(const Column* cols, int n, Rectangle r, ColLayout* L, float flex_min);
bool table_header(Rectangle r, const Column* cols, const ColLayout* L, int* sort, bool* desc);
void table_cell(Rectangle row, const ColLayout* L, int col, const char* s, Color c, int weight, int align);
Rectangle table_cell_rect(Rectangle row, const ColLayout* L, int col);
/* Clamps scroll, handles the wheel and draws the scrollbar; returns first row. */
int  table_scroll(Rectangle body, float* scroll, int rows, float row_h, uint32_t id);

/* Draws the page title; shrinks *r to the content area and returns the free
 * toolbar strip to the right of the title (cut buttons off its right edge). */
Rectangle page_header(Rectangle* r, const char* title, const char* subtitle);
void   card(Rectangle r);
void   stat_item(float x, float y, float w, const char* label, const char* value, Color accent);
void   empty_state(Rectangle r, int icon, const char* text);
void   legend_dot(float x, float cy, Color c, const char* label);

/* window stats over the visible range */
typedef struct { float avg, peak, p95, last; } WinStats;
WinStats win_stats(const float* ring, uint64_t first, uint64_t last);

/* ---- pages -------------------------------------------------------------------- */

void page_processes(Rectangle r);
void page_performance(Rectangle r);
void page_analysis(Rectangle r);
void page_network(Rectangle r);
void page_disk(Rectangle r);
void page_memory(Rectangle r);
void page_startup(Rectangle r);
void page_users(Rectangle r);
void page_services(Rectangle r);
void page_settings(Rectangle r);
void page_firmware(Rectangle r);
void page_events(Rectangle r);

void open_run_dialog(void);
void open_affinity_dialog(const Proc* p);
void open_confirm(int action, const Proc* p);
void settings_save(void);

/* ---- menubar.c (Hisashi hoswl integration) ----------------------------------- */

void menubar_init(bool enabled);
void menubar_frame(void);
void menubar_set_enabled(bool on);
bool menubar_connected(void);
void menubar_shutdown(void);

#endif /* TARMAN_APP_H */
