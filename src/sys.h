#ifndef TARMAN_SYS_H
#define TARMAN_SYS_H

/* The system model. Pure data, no <windows.h> and no raylib: sys_win.c fills
 * it from a sampler thread, the UI reads it under sys_lock(). Keeping the two
 * headers apart is not just tidiness -- raylib and windows.h both define
 * Rectangle, DrawText, CloseWindow, LoadImage and friends, so the translation
 * units that talk to Win32 must never see raylib.h and vice versa.
 *
 * Time model: the sampler records one sample per second. Sample k (counting
 * from 0 since start) lives in every ring at index k % HIST_LEN, so all rings
 * -- system-wide and per-process -- are aligned on the same clock and one
 * cursor position means the same instant everywhere. HIST_LEN samples is the
 * ten-minute moving window. */

#include <stdbool.h>
#include <stdint.h>

#define HIST_LEN     600   /* 10 minutes at 1 Hz */
#define MAX_CORES    256
#define MAX_DISKS    16
#define MAX_NETS     16
#define MAX_GPUS     8
#define MAX_VOLUMES  26
#define MAX_SESSIONS 64
#define GPU_ENGINES  4     /* 3D, Copy, Video Encode, Video Decode */

typedef struct { float v[HIST_LEN]; } Hist;

static inline float hist_at(const float* ring, uint64_t k) { return ring[k % HIST_LEN]; }

/* ---- processes ---------------------------------------------------------- */

typedef enum { PGROUP_APP = 0, PGROUP_BACKGROUND, PGROUP_WINDOWS, PGROUP_COUNT } ProcGroup;

typedef struct {
    uint32_t pid, ppid;
    uint64_t create_time;      /* FILETIME ticks (100ns since 1601), 0 = boot */
    char     name[64];         /* image name, UTF-8                          */
    char     desc[128];        /* FileDescription, falls back to name        */
    char     company[96];
    char     path[300];
    char     user[64];
    char     cmdline[1024];
    char     title[128];       /* first visible top-level window, apps only  */
    bool     meta_done;        /* path/desc/user/icon resolved by the worker */
    int      icon;             /* index into SysState.icons, -1 = none       */

    uint32_t session, threads, handles;
    int32_t  base_prio;
    uint64_t ws_private, ws, ws_peak, commit, virt, paged_pool, nonpaged_pool;
    uint32_t page_faults, hard_faults;
    uint64_t cpu_time;                       /* user+kernel, 100ns           */
    uint64_t io_read, io_write, io_other;    /* cumulative bytes             */
    uint64_t gpu_mem;                        /* dedicated bytes              */

    float    cpu;                            /* % of the whole machine       */
    float    gpu;                            /* % of the busiest engine      */
    float    io_read_bps, io_write_bps, io_other_bps;
    float    hard_faults_ps;

    bool     alive, is_app, is_suspended;
    int      group;                          /* ProcGroup                    */
    uint32_t seen;                           /* worker bookkeeping           */
    uint64_t first_sample, last_sample;      /* valid ring range             */

    float    h_cpu[HIST_LEN];
    float    h_mem[HIST_LEN];                /* private working set, bytes   */
    float    h_io[HIST_LEN];                 /* read+write, bytes/s          */
    float    h_gpu[HIST_LEN];
} Proc;

/* On-demand detail for one process (selection panel). */
typedef struct {
    bool     ok;
    uint32_t prio_class;          /* Win32 *_PRIORITY_CLASS value, 0 = unknown */
    uint32_t gdi, user_objs;
    uint64_t affinity, sys_affinity;
    bool     efficiency;
    bool     wow64;
} ProcExtra;

typedef struct {
    char     name[64];
    char     path[300];
    uint64_t base, size;
} ModuleInfo;

/* An icon decoded to RGBA by the worker; the UI turns it into a texture. */
typedef struct {
    uint32_t hash;
    int      w, h;
    uint8_t* rgba;
} IconEntry;

/* ---- devices ------------------------------------------------------------ */

typedef struct {
    char     name[96];
    int      sockets, cores, logical, numa;
    uint32_t base_mhz;
    uint64_t l1, l2, l3;
    bool     virtualization;
    float    usage, kernel, mhz;
    float    core[MAX_CORES];
    bool     temp_ok;             /* a temperature source was found     */
    float    temp;                /* degrees C                          */
    char     temp_src[48];        /* "ACPI thermal zone", "HWiNFO", ... */
    Hist     h_usage, h_kernel, h_temp;
    Hist     h_core[MAX_CORES];
} CpuInfo;

typedef struct {
    uint64_t total, avail, used, installed, hw_reserved;
    uint64_t commit, commit_limit, cached, standby, modified, free;
    uint64_t paged_pool, nonpaged_pool, compressed;
    float    load;
    float    hard_faults_ps;
    uint32_t speed_mhz;
    int      slots_used, slots_total;
    char     form[24], type[16];
    Hist     h_used;          /* bytes in use                          */
    Hist     h_commit;        /* committed bytes                       */
    Hist     h_hard_faults;   /* hard faults / s, summed over processes */
} MemInfo;

typedef struct {
    int      index;
    char     name[48];        /* "Disk 0"           */
    char     letters[32];     /* "C: D:"            */
    char     model[96];
    char     kind[8];         /* SSD / HDD / ""     */
    uint64_t capacity;
    bool     system;
    float    active, read_bps, write_bps, resp_ms, queue;
    Hist     h_active, h_read, h_write;
} DiskInfo;

typedef struct {
    char     root[4];         /* "C:\"              */
    char     label[64];
    char     fs[16];
    uint64_t total, free;
    int      kind;            /* 0 fixed, 1 removable, 2 network, 3 cdrom, 4 ram */
} VolumeInfo;

typedef struct {
    uint64_t luid;            /* interface LUID */
    char     name[64];        /* "Wi-Fi", "Ethernet" */
    char     desc[128];
    char     kind[16];
    char     ipv4[48], ipv6[64], mac[24], dns[48];
    uint64_t link_bps;
    uint64_t in_octets, out_octets;
    float    recv_bps, send_bps;
    Hist     h_recv, h_send;
} NetInfo;

static const char* const GPU_ENGINE_NAMES[GPU_ENGINES] = { "3D", "Copy", "Video Encode", "Video Decode" };

typedef struct {
    char     name[128];
    uint32_t luid_lo;
    int32_t  luid_hi;
    uint64_t ded_total, ded_used, shared_total, shared_used;
    float    util;
    float    eng[GPU_ENGINES];
    bool     temp_ok;
    float    temp;                /* degrees C (D3DKMT, as Task Manager) */
    uint32_t fan_rpm;
    float    power_pct;           /* % of the board's power budget       */
    uint64_t mem_freq;            /* Hz                                  */
    Hist     h_util, h_ded, h_shared, h_temp;
    Hist     h_eng[GPU_ENGINES];
} GpuInfo;

/* ---- lists ---------------------------------------------------------------- */

typedef struct {
    uint8_t  udp, v6;
    uint32_t pid;
    char     local[48], remote[48];
    uint16_t lport, rport;
    int      state;           /* MIB_TCP_STATE_*; 0 for UDP */
} ConnInfo;

typedef struct {
    char     name[96];
    char     display[160];
    uint32_t pid;
    int      state;           /* SERVICE_STOPPED=1 ... SERVICE_PAUSED=7 */
    int      start_type;      /* SERVICE_BOOT_START=0 ... SERVICE_DISABLED=4, -1 unknown */
    bool     delayed;
} ServiceInfo;

typedef enum {
    STARTUP_HKCU_RUN = 0, STARTUP_HKLM_RUN, STARTUP_HKLM_RUN32,
    STARTUP_USER_FOLDER, STARTUP_COMMON_FOLDER,
} StartupSource;

typedef struct {
    char name[128];           /* registry value name, or .lnk file name */
    char display[128];
    char command[600];
    char target[300];         /* resolved exe, when known */
    char publisher[96];
    int  source;              /* StartupSource */
    bool enabled;
} StartupItem;

typedef struct {
    uint32_t id;
    char     user[64];
    char     station[32];
    char     state[16];
} SessionInfo;

/* ---- firmware (read on demand by the Firmware page) ----------------------- */

typedef struct {
    char     locator[32], bank[32], manufacturer[48], part[48], serial[32];
    char     type[16], form[16];
    uint64_t size;
    uint16_t speed, cfg_speed, voltage_mv;
} MemSlot;

typedef struct {
    uint16_t id;
    char     name[96];
    bool     active, current;
} BootEntry;

typedef struct {
    bool     ok;
    int64_t  read_ms;
    char     fw_type[24];
    char     bios_vendor[64], bios_version[64], bios_date[32], bios_release[16], ec_release[16];
    uint32_t rom_kb;
    bool     ch_uefi, ch_vm, ch_acpi, ch_usb_legacy, ch_boot_cd, ch_flash, ch_shadow, ch_select_boot;
    char     sys_manufacturer[64], sys_product[64], sys_version[64], sys_serial[64], sys_uuid[40];
    char     sys_sku[64], sys_family[64], wake[24];
    char     board_manufacturer[64], board_product[64], board_version[64], board_serial[64];
    char     chassis_manufacturer[64], chassis_type[32];
    char     cpu_socket[48], cpu_version[96], cpu_manufacturer[48];
    uint16_t cpu_max_mhz, cpu_cur_mhz, cpu_ext_clock;
    float    cpu_voltage;
    int      cpu_cores, cpu_threads;
    MemSlot  slots[16];
    int      nslots, mem_array_slots;
    uint64_t mem_max_capacity;
    char     mem_ecc[24];
    int      secure_boot, setup_mode, audit_mode, deployed_mode;   /* -1 unknown */
    bool     vars_ok;
    char     vars_err[128];
    uint16_t boot_current;
    int      boot_timeout;                                         /* -1 unknown */
    char     platform_lang[16];
    bool     fw_ui_supported;
    BootEntry boot[24];
    int      nboot;
    int      tpm_version;                                          /* 0 none, 1 = 1.2, 2 = 2.0 */
    char     tpm_iface[24];
    bool     virt_firmware, hypervisor, nx, vbs_cfg, hvci_cfg, sse42, avx2, avx512;
    char     hypervisor_vendor[16];
} FirmwareInfo;

void sys_firmware_read(FirmwareInfo* fw);

/* ---- custom title bar ------------------------------------------------------- */

/* Subclasses the window: the caption is removed (client area covers it),
 * but Windows keeps doing drag, Aero Snap, Win11 snap layouts on the
 * maximize button and edge resize. Rects are physical client pixels. */
void sys_titlebar_install(void* hwnd);
void sys_titlebar_update(int caption_h, const int max_btn[4], const int (*client)[4], int nclient);
bool sys_titlebar_max_hover(void);
bool sys_titlebar_max_down(void);
bool sys_window_active(void);

/* ---- activity log ------------------------------------------------------------
 *
 * Tarman's own log: actions taken, failures, and process starts/exits seen
 * by the sampler. Kept in memory for the Events page and appended to
 * %APPDATA%\Tarman\logs\tarman-YYYY-MM-DD.log (actions and errors only;
 * process churn would drown the file). */

enum { LOG_LV_ERROR = 2, LOG_LV_WARN = 3, LOG_LV_INFO = 4 };

typedef struct {
    int64_t  time_ms;
    uint8_t  level;
    char     cat[16];
    char     text[220];
    uint32_t pid;
} ActivityEntry;

#define ACTIVITY_MAX 4096

void sys_log(int level, const char* cat, uint32_t pid, bool to_file, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));
bool sys_log_dir(char* out, int n);

/* ---- Windows event logs (Events page) -----------------------------------------
 *
 * Queries run on a background thread so a slow log never stalls the UI; the
 * page reads the result under sys_events_lock(). */

typedef struct {
    uint64_t record_id;
    int64_t  time_ms;
    uint32_t event_id;
    uint8_t  level;            /* 1 critical, 2 error, 3 warning, 4 info, 5 verbose (0 shown as info) */
    uint32_t pid, tid;
    char     provider[96];
    char     task[48];
    char     computer[64];
    char*    message;          /* malloc'd UTF-8, may be NULL until formatted */
} EventRec;

typedef struct {
    char     name[160];
    uint64_t records;
} EventChannel;

typedef struct {
    EventRec*     recs;
    int           n;
    bool          busy;
    bool          truncated;   /* hit the row cap */
    char          channel[160];
    char          error[200];
    EventChannel* channels;    /* non-empty "Applications and Services" logs */
    int           nchannels;
    bool          channels_busy;
} EventsState;

/* level_mask: bit n set = include level n (bit 0 = "LogAlways", shown as info) */
void         sys_events_query(const char* channel, unsigned level_mask, int hours, int max_rows);
void         sys_events_list_channels(void);
EventsState* sys_events_lock(void);
void         sys_events_unlock(void);
bool         sys_event_xml(const char* channel, uint64_t record_id, char* out, int n);
uint64_t     sys_event_log_count(const char* channel);

/* ---- the whole picture ---------------------------------------------------- */

typedef struct {
    uint64_t samples;                  /* samples recorded so far          */
    int64_t  sample_time[HIST_LEN];    /* unix ms of each sample           */
    bool     paused;

    CpuInfo  cpu;
    MemInfo  mem;
    DiskInfo disk[MAX_DISKS];      int ndisk;
    VolumeInfo vol[MAX_VOLUMES];   int nvol;
    NetInfo  net[MAX_NETS];        int nnet;
    GpuInfo  gpu[MAX_GPUS];        int ngpu;

    Proc**   procs;  int nproc, proc_cap;   /* alive + exited within window */
    int      total_procs, total_threads, total_handles;

    ConnInfo*    conns;   int nconn;
    ServiceInfo* svcs;    int nsvc;
    StartupItem* startup; int nstartup;
    SessionInfo  sessions[MAX_SESSIONS]; int nsession;

    IconEntry* icons; int nicon, icon_cap;

    ActivityEntry* activity;      /* ring of ACTIVITY_MAX */
    int      act_head, act_count;
    uint32_t act_gen;             /* bumps on every append */

    uint64_t uptime_ms;
    char     computer[64], os_name[96], user[64];
    bool     elevated;
    uint32_t self_pid;
} SysState;

/* Interest flags: the UI tells the worker which slow lists it is looking at
 * so services and connections are only polled while visible. */
enum { SYSWANT_SERVICES = 1, SYSWANT_CONNS = 2, SYSWANT_STARTUP = 4, SYSWANT_SESSIONS = 8 };

/* Demo mode: a synthetic machine (fake process tree, user "demo", computer
 * "DEMO-PC", made-up event logs) for screenshots that show no real data.
 * Every process action refuses while it is on. Call before sys_start(). */
void      sys_set_demo(bool on);
bool      sys_is_demo(void);

SysState* sys_start(void);
void      sys_stop(void);
void      sys_lock(void);
void      sys_unlock(void);
void      sys_set_paused(bool paused);
void      sys_want(int flags);           /* call every frame for what is on screen */
void      sys_refresh_startup(void);     /* force a re-read on next tick */

/* Actions. Each returns true on success; on failure `err` gets a short,
 * human-readable reason ("Access is denied." etc.). */
bool sys_kill(uint32_t pid, char* err, int errn);
bool sys_kill_tree(uint32_t pid, char* err, int errn);
bool sys_suspend(uint32_t pid, bool resume, char* err, int errn);
bool sys_set_priority(uint32_t pid, uint32_t prio_class, char* err, int errn);
bool sys_set_affinity(uint32_t pid, uint64_t mask, char* err, int errn);
bool sys_set_efficiency(uint32_t pid, bool on, char* err, int errn);
bool sys_create_dump(uint32_t pid, const char* name, char* out_path, int outn, char* err, int errn);
bool sys_run(const char* cmd, bool admin, char* err, int errn);
bool sys_service_control(const char* name, int op, char* err, int errn); /* 0 start,1 stop,2 restart */
bool sys_startup_set(const StartupItem* it, bool enabled, char* err, int errn);
bool sys_session_action(uint32_t id, int op, char* err, int errn);      /* 0 disconnect, 1 sign off */
void sys_open_location(const char* path);
void sys_open_properties(const char* path);
void sys_search_online(const char* name);
void sys_open_tool(const char* tool);
bool sys_restart_elevated(void);       /* true: the elevated copy launched */

void sys_proc_extra(uint32_t pid, ProcExtra* out);
int  sys_modules(uint32_t pid, ModuleInfo* out, int max);

void    sys_window_setup(void* hwnd, bool dark, bool topmost);
int64_t sys_now_ms(void);
void    sys_format_clock(int64_t unix_ms, char* out, int n);      /* "14:32:05" */
void    sys_format_filetime(uint64_t ft, char* out, int n);       /* local date+time */
void    sys_attach_console(void);
bool    sys_settings_path(char* out, int n);

#endif /* TARMAN_SYS_H */
