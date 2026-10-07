/* Win32 side of Tarman: the sampler thread that fills SysState once a second,
 * plus every action the UI can take on a process, service, startup entry or
 * session. This is the only translation unit that includes <windows.h>; it
 * never sees raylib.h (see the note at the top of sys.h).
 *
 * Data sources, cheapest first:
 *   NtQuerySystemInformation  every process with CPU times, memory, I/O and
 *                             thread states in one call -- no OpenProcess
 *                             per process, so protected processes show too.
 *   NtQSI per-processor       per-core idle/kernel/user times.
 *   PDH (English names)       disks, GPU engines/memory, standby/modified
 *                             lists, CPU performance %. English counter names
 *                             keep this working on non-English Windows.
 *   GetIfTable2               per-adapter octet counters.
 *   iphlpapi / SCM / WTS      connections, services, sessions (only while the
 *                             UI is looking at them, see sys_want). */

#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#define UNICODE
#define _UNICODE

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <sddl.h>
#include <dxgi.h>
#include <wbemidl.h>

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sys.h"

/* ---- native structures (x64 layouts) ----------------------------------- */

typedef struct {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    HANDLE        UniqueProcess, UniqueThread;
    LONG          Priority, BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
} TmThreadInfo;

typedef struct {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
    ULONG          HandleCount;
    ULONG          SessionId;
    ULONG_PTR      UniqueProcessKey;
    SIZE_T         PeakVirtualSize, VirtualSize;
    ULONG          PageFaultCount;
    SIZE_T         PeakWorkingSetSize, WorkingSetSize;
    SIZE_T         QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T         QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER  ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER  ReadTransferCount, WriteTransferCount, OtherTransferCount;
} TmProcInfo;

typedef struct {
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG         InterruptCount;
} TmCpuPerf;

typedef struct {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} TmPowerInfo;

typedef struct {
    ULONG Version, ControlMask, StateMask;
} TmThrottle;

typedef LONG (NTAPI *PFN_NtQSI)(ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *PFN_NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *PFN_NtProc)(HANDLE);
typedef LONG (WINAPI *PFN_CallNtPower)(int, PVOID, ULONG, PVOID, ULONG);
typedef BOOL (WINAPI *PFN_ProcInfo)(HANDLE, int, LPVOID, DWORD);

static PFN_NtQSI    pNtQSI;
static PFN_NtQIP    pNtQIP;
static PFN_NtProc   pNtSuspend, pNtResume;
static PFN_ProcInfo pSetProcInfo, pGetProcInfo;

#define STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004)
#define TM_ProcessPowerThrottling   4
#define TM_ProcessCommandLine       60

/* ---- shared state ------------------------------------------------------- */

static SysState         g_st;
static CRITICAL_SECTION g_cs;
static HANDLE           g_thread;
static volatile LONG    g_quit;
static volatile LONG    g_want;
static volatile LONG    g_startup_dirty = 1;
static uint32_t         g_gen;
static char             g_windir[MAX_PATH]; /* lowercase, trailing backslash */

void sys_lock(void)   { EnterCriticalSection(&g_cs); }
void sys_unlock(void) { LeaveCriticalSection(&g_cs); }
void sys_want(int flags) { InterlockedExchange(&g_want, flags); }
void sys_refresh_startup(void) { InterlockedExchange(&g_startup_dirty, 1); }

void sys_set_paused(bool paused)
{
    sys_lock();
    g_st.paused = paused;
    sys_unlock();
}

/* ---- small helpers ------------------------------------------------------ */

static void w2u(const wchar_t* w, char* out, int n)
{
    if (!n) return;
    out[0] = 0;
    if (!w) return;
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    if (r <= 0) { out[n - 1] = 0; if (!out[0]) out[0] = 0; }
    out[n - 1] = 0;
}

static void w2u_len(const wchar_t* w, int wlen, char* out, int n)
{
    out[0] = 0;
    if (!w || wlen <= 0) return;
    int r = WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, n - 1, NULL, NULL);
    out[r > 0 ? r : 0] = 0;
}

static void u2w(const char* s, wchar_t* out, int n)
{
    out[0] = 0;
    if (!s) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    out[n - 1] = 0;
}

static void set_err(char* err, int errn, DWORD code)
{
    if (!err || errn <= 0) return;
    wchar_t buf[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             buf, 256, NULL);
    if (!n) { snprintf(err, errn, "Error %lu", (unsigned long)code); return; }
    while (n && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r' || buf[n - 1] == L' ')) buf[--n] = 0;
    w2u(buf, err, errn);
}

static void set_msg(char* err, int errn, const char* msg)
{
    if (err && errn > 0) snprintf(err, errn, "%s", msg);
}

static uint32_t hash_str(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)(*s | 0x20)) * 16777619u;
    return h ? h : 1;
}

static void lower_ascii(char* s)
{
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static bool starts_with_ci(const char* s, const char* prefix)
{
    for (; *prefix; ++s, ++prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void trim(char* s)
{
    char* p = s;
    while (*p == ' ') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

int64_t sys_now_ms(void)
{
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (int64_t)((ft64(f) - 116444736000000000ULL) / 10000ULL);
}

void sys_format_clock(int64_t unix_ms, char* out, int n)
{
    uint64_t t = (uint64_t)unix_ms * 10000ULL + 116444736000000000ULL;
    FILETIME f = { (DWORD)t, (DWORD)(t >> 32) }, lf;
    SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &s);
    snprintf(out, n, "%02d:%02d:%02d", s.wHour, s.wMinute, s.wSecond);
}

void sys_format_filetime(uint64_t t, char* out, int n)
{
    if (!t) { snprintf(out, n, "At boot"); return; }
    FILETIME f = { (DWORD)t, (DWORD)(t >> 32) }, lf;
    SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &s);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
             s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
}

void sys_attach_console(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

bool sys_settings_path(char* out, int n)
{
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) return false;
    wcsncat(dir, L"\\Tarman", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    wcsncat(dir, L"\\settings.ini", MAX_PATH - wcslen(dir) - 1);
    w2u(dir, out, n);
    return true;
}

static bool reg_str(HKEY root, const wchar_t* key, const wchar_t* val, char* out, int n)
{
    wchar_t buf[512];
    DWORD sz = sizeof buf;
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return false;
    w2u(buf, out, n);
    return true;
}

/* ---- activity log ----------------------------------------------------------- */

bool sys_log_dir(char* out, int n)
{
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) return false;
    wcsncat(dir, L"\\Tarman", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    wcsncat(dir, L"\\logs", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    w2u(dir, out, n);
    return true;
}

static void log_to_file(const ActivityEntry* e)
{
    static char dir[MAX_PATH * 3];
    if (!dir[0] && !sys_log_dir(dir, sizeof dir)) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char path[MAX_PATH * 3 + 40];
    snprintf(path, sizeof path, "%s\\tarman-%04d-%02d-%02d.log", dir, t.wYear, t.wMonth, t.wDay);
    wchar_t wp[MAX_PATH * 2];
    u2w(path, wp, MAX_PATH * 2);
    HANDLE h = CreateFileW(wp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[400];
    const char* lv = e->level <= LOG_LV_ERROR ? "ERROR" : e->level == LOG_LV_WARN ? "WARN " : "INFO ";
    int len = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d  %s  %-9s %s\r\n", t.wYear, t.wMonth,
                       t.wDay, t.wHour, t.wMinute, t.wSecond, lv, e->cat, e->text);
    DWORD wrote;
    if (len > 0) WriteFile(h, line, (DWORD)(len < (int)sizeof line ? len : (int)sizeof line - 1), &wrote, NULL);
    CloseHandle(h);
}

void sys_log(int level, const char* cat, uint32_t pid, bool to_file, const char* fmt, ...)
{
    ActivityEntry e;
    memset(&e, 0, sizeof e);
    e.time_ms = sys_now_ms();
    e.level = (uint8_t)level;
    e.pid = pid;
    snprintf(e.cat, sizeof e.cat, "%s", cat);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    /* the critical section is re-entrant, so callers already holding it are fine */
    EnterCriticalSection(&g_cs);
    if (!g_st.activity) g_st.activity = calloc(ACTIVITY_MAX, sizeof *g_st.activity);
    if (g_st.activity) {
        g_st.activity[g_st.act_head] = e;
        g_st.act_head = (g_st.act_head + 1) % ACTIVITY_MAX;
        if (g_st.act_count < ACTIVITY_MAX) g_st.act_count++;
        g_st.act_gen++;
    }
    LeaveCriticalSection(&g_cs);
    if (to_file) log_to_file(&e);
}

/* ---- static machine info ------------------------------------------------ */

static void enable_debug_privilege(void)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp = { 1 };
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, NULL, NULL);
    }
    CloseHandle(tok);
}

static bool is_elevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION e = { 0 };
    DWORD sz = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    GetTokenInformation(tok, TokenElevation, &e, sizeof e, &sz);
    CloseHandle(tok);
    return e.TokenIsElevated != 0;
}

static void init_cpu_static(CpuInfo* c)
{
    if (!reg_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                 L"ProcessorNameString", c->name, sizeof c->name))
        snprintf(c->name, sizeof c->name, "Processor");
    trim(c->name);

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    uint8_t* buf = malloc(len ? len : 1);
    if (buf && GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            switch (e->Relationship) {
            case RelationProcessorCore:    c->cores++; break;
            case RelationProcessorPackage: c->sockets++; break;
            case RelationNumaNode:         c->numa++; break;
            case RelationCache:
                if (e->Cache.Level == 1) c->l1 += e->Cache.CacheSize;
                if (e->Cache.Level == 2) c->l2 += e->Cache.CacheSize;
                if (e->Cache.Level == 3) c->l3 += e->Cache.CacheSize;
                break;
            default: break;
            }
            off += e->Size;
        }
    }
    free(buf);
    c->logical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (c->logical > MAX_CORES) c->logical = MAX_CORES;
    if (c->logical < 1) c->logical = 1;
    c->virtualization = IsProcessorFeaturePresent(21 /* PF_VIRT_FIRMWARE_ENABLED */) != 0;

    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    PFN_CallNtPower call = pp ? (PFN_CallNtPower)(void*)GetProcAddress(pp, "CallNtPowerInformation") : NULL;
    if (call) {
        TmPowerInfo* pi = calloc((size_t)c->logical, sizeof *pi);
        if (pi && call(11 /* ProcessorInformation */, NULL, 0, pi, (ULONG)(sizeof *pi * c->logical)) == 0)
            c->base_mhz = pi[0].MaxMhz;
        free(pi);
    }
    if (!c->base_mhz) {
        DWORD mhz = 0, sz = sizeof mhz;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &sz);
        c->base_mhz = mhz;
    }
}

/* SMBIOS type 17 (Memory Device) gives slots, speed and form factor -- the
 * same table Task Manager reads. */
static void init_mem_static(MemInfo* m)
{
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb)) m->installed = kb * 1024ULL;

    UINT sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (!sz) return;
    uint8_t* buf = malloc(sz);
    if (!buf) return;
    if (GetSystemFirmwareTable('RSMB', 0, buf, sz) != sz) { free(buf); return; }
    uint32_t tlen = *(uint32_t*)(buf + 4);
    uint8_t* p = buf + 8;
    uint8_t* end = p + tlen;
    if (end > buf + sz) end = buf + sz;
    while (p + 4 <= end) {
        uint8_t type = p[0], hlen = p[1];
        if (hlen < 4 || p + hlen > end) break;
        if (type == 17 && hlen >= 0x17) {
            uint16_t size = *(uint16_t*)(p + 0x0C);
            m->slots_total++;
            if (size != 0 && size != 0xFFFF) {
                m->slots_used++;
                uint8_t ff = p[0x0E], mt = p[0x12];
                uint16_t speed = *(uint16_t*)(p + 0x15);
                if (hlen >= 0x22) {
                    uint16_t cfg = *(uint16_t*)(p + 0x20);
                    if (cfg && cfg != 0xFFFF) speed = cfg;
                }
                if (speed && speed != 0xFFFF && speed > m->speed_mhz) m->speed_mhz = speed;
                const char* f = ff == 0x09 ? "DIMM" : ff == 0x0D ? "SODIMM" : ff == 0x0F ? "FB-DIMM"
                              : ff == 0x0B ? "Row of chips" : "Other";
                snprintf(m->form, sizeof m->form, "%s", f);
                const char* t = mt == 0x18 ? "DDR3" : mt == 0x1A ? "DDR4" : mt == 0x1B ? "LPDDR"
                              : mt == 0x1C ? "LPDDR2" : mt == 0x1D ? "LPDDR3" : mt == 0x1E ? "LPDDR4"
                              : mt == 0x22 ? "DDR5" : mt == 0x23 ? "LPDDR5" : "";
                snprintf(m->type, sizeof m->type, "%s", t);
            }
        }
        /* skip the formatted area and the double-NUL terminated string set */
        uint8_t* s = p + hlen;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
        p = s + 2;
    }
    free(buf);
}

static void disk_static(DiskInfo* d)
{
    wchar_t path[64];
    _snwprintf(path, 64, L"\\\\.\\PhysicalDrive%d", d->index);
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery };
    uint8_t buf[1024];
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR* dd = (STORAGE_DEVICE_DESCRIPTOR*)buf;
        char vendor[64] = "", product[64] = "";
        if (dd->VendorIdOffset && dd->VendorIdOffset < got)
            snprintf(vendor, sizeof vendor, "%s", (char*)buf + dd->VendorIdOffset);
        if (dd->ProductIdOffset && dd->ProductIdOffset < got)
            snprintf(product, sizeof product, "%s", (char*)buf + dd->ProductIdOffset);
        trim(vendor); trim(product);
        if (vendor[0] && !strstr(product, vendor))
            snprintf(d->model, sizeof d->model, "%s %s", vendor, product);
        else
            snprintf(d->model, sizeof d->model, "%s", product);
        bool nvme = dd->BusType == BusTypeNvme;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp = { 0 };
        STORAGE_PROPERTY_QUERY q2 = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery };
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof q2, &sp, sizeof sp, &got, NULL))
            snprintf(d->kind, sizeof d->kind, "%s", sp.IncursSeekPenalty ? "HDD" : (nvme ? "NVMe" : "SSD"));
        else if (nvme)
            snprintf(d->kind, sizeof d->kind, "NVMe");
        if (dd->BusType == BusTypeUsb && !d->kind[0]) snprintf(d->kind, sizeof d->kind, "USB");
    }
    DISK_GEOMETRY_EX geo;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof geo, &got, NULL))
        d->capacity = (uint64_t)geo.DiskSize.QuadPart;
    CloseHandle(h);
}

static const GUID TM_IID_IDXGIFactory1 =
    { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };

static void init_gpu_static(SysState* st)
{
    HMODULE dx = LoadLibraryW(L"dxgi.dll");
    if (!dx) return;
    typedef HRESULT (WINAPI *PFN_Create)(REFIID, void**);
    PFN_Create create = (PFN_Create)(void*)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1* f = NULL;
    if (!create || FAILED(create(&TM_IID_IDXGIFactory1, (void**)&f)) || !f) return;
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; st->ngpu < MAX_GPUS && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            bool dup = false;
            for (int g = 0; g < st->ngpu; ++g)
                if (st->gpu[g].luid_lo == d.AdapterLuid.LowPart && st->gpu[g].luid_hi == d.AdapterLuid.HighPart) dup = true;
            if (!dup) {
                GpuInfo* g = &st->gpu[st->ngpu++];
                w2u(d.Description, g->name, sizeof g->name);
                g->luid_lo      = d.AdapterLuid.LowPart;
                g->luid_hi      = d.AdapterLuid.HighPart;
                g->ded_total    = d.DedicatedVideoMemory;
                g->shared_total = d.SharedSystemMemory;
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

static void init_os_static(SysState* st)
{
    wchar_t w[128];
    DWORD n = 128;
    if (GetComputerNameW(w, &n)) w2u(w, st->computer, sizeof st->computer);
    n = 128;
    if (GetUserNameW(w, &n)) w2u(w, st->user, sizeof st->user);

    char prod[64] = "Windows", disp[32] = "", build[16] = "";
    const wchar_t* k = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    reg_str(HKEY_LOCAL_MACHINE, k, L"ProductName", prod, sizeof prod);
    reg_str(HKEY_LOCAL_MACHINE, k, L"DisplayVersion", disp, sizeof disp);
    reg_str(HKEY_LOCAL_MACHINE, k, L"CurrentBuild", build, sizeof build);
    /* ProductName still says "Windows 10" on 11; the build number does not lie. */
    char* ten = strstr(prod, "Windows 10");
    if (ten && atoi(build) >= 22000) ten[9] = '1';
    snprintf(st->os_name, sizeof st->os_name, "%s %s (build %s)", prod, disp, build);

    wchar_t wd[MAX_PATH];
    GetWindowsDirectoryW(wd, MAX_PATH);
    w2u(wd, g_windir, sizeof g_windir);
    lower_ascii(g_windir);
    size_t l = strlen(g_windir);
    if (l && g_windir[l - 1] != '\\' && l + 1 < sizeof g_windir) { g_windir[l] = '\\'; g_windir[l + 1] = 0; }
}

/* ---- PDH ------------------------------------------------------------------ */

static PDH_HQUERY   g_pdh;
static PDH_HCOUNTER c_perf, c_dk_idle, c_dk_read, c_dk_write, c_dk_sec, c_dk_queue;
static PDH_HCOUNTER c_sb_res, c_sb_norm, c_sb_core, c_modified, c_freezero;
static PDH_HCOUNTER c_gpu_eng, c_gpu_ded, c_gpu_shared, c_gpu_pmem, c_thermal;

static PDH_HCOUNTER pdh_add(const wchar_t* path)
{
    PDH_HCOUNTER c = NULL;
    if (PdhAddEnglishCounterW(g_pdh, path, 0, &c) != ERROR_SUCCESS) return NULL;
    return c;
}

static void pdh_init(void)
{
    if (PdhOpenQueryW(NULL, 0, &g_pdh) != ERROR_SUCCESS) { g_pdh = NULL; return; }
    c_perf      = pdh_add(L"\\Processor Information(_Total)\\% Processor Performance");
    c_dk_idle   = pdh_add(L"\\PhysicalDisk(*)\\% Idle Time");
    c_dk_read   = pdh_add(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
    c_dk_write  = pdh_add(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
    c_dk_sec    = pdh_add(L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer");
    c_dk_queue  = pdh_add(L"\\PhysicalDisk(*)\\Current Disk Queue Length");
    c_sb_res    = pdh_add(L"\\Memory\\Standby Cache Reserve Bytes");
    c_sb_norm   = pdh_add(L"\\Memory\\Standby Cache Normal Priority Bytes");
    c_sb_core   = pdh_add(L"\\Memory\\Standby Cache Core Bytes");
    c_modified  = pdh_add(L"\\Memory\\Modified Page List Bytes");
    c_freezero  = pdh_add(L"\\Memory\\Free & Zero Page List Bytes");
    c_gpu_eng   = pdh_add(L"\\GPU Engine(*)\\Utilization Percentage");
    c_gpu_ded   = pdh_add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    c_gpu_shared= pdh_add(L"\\GPU Adapter Memory(*)\\Shared Usage");
    c_gpu_pmem  = pdh_add(L"\\GPU Process Memory(*)\\Dedicated Usage");
    c_thermal   = pdh_add(L"\\Thermal Zone Information(*)\\High Precision Temperature");
    PdhCollectQueryData(g_pdh);
}

static double pdh_value(PDH_HCOUNTER c)
{
    if (!c) return 0;
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &v) != ERROR_SUCCESS) return 0;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return 0;
    return v.doubleValue;
}

/* Returns a malloc'd item array (caller frees) and its length. */
static PDH_FMT_COUNTERVALUE_ITEM_W* pdh_array(PDH_HCOUNTER c, DWORD fmt, DWORD* count)
{
    *count = 0;
    if (!c) return NULL;
    DWORD bytes = 0, n = 0;
    if (PdhGetFormattedCounterArrayW(c, fmt, &bytes, &n, NULL) != (PDH_STATUS)PDH_MORE_DATA || !bytes) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = malloc(bytes);
    if (!items) return NULL;
    if (PdhGetFormattedCounterArrayW(c, fmt, &bytes, &n, items) != ERROR_SUCCESS) { free(items); return NULL; }
    *count = n;
    return items;
}

static bool parse_luid(const wchar_t* s, int32_t* hi, uint32_t* lo)
{
    const wchar_t* p = wcsstr(s, L"luid_0x");
    if (!p) return false;
    wchar_t* e;
    *hi = (int32_t)wcstoul(p + 7, &e, 16);
    if (wcsncmp(e, L"_0x", 3) != 0) return false;
    *lo = (uint32_t)wcstoul(e + 3, NULL, 16);
    return true;
}

static int gpu_find(const SysState* st, int32_t hi, uint32_t lo)
{
    for (int i = 0; i < st->ngpu; ++i)
        if (st->gpu[i].luid_hi == hi && st->gpu[i].luid_lo == lo) return i;
    return -1;
}

/* ---- temperatures -------------------------------------------------------------
 *
 * GPU: D3DKMT adapter perf data -- the same driver-reported values Task
 * Manager shows (temperature, fan, power), vendor-neutral.
 * CPU: Windows has no driver-free CPU sensor API. In order we try ACPI
 * thermal zones (PDH; many desktops expose none), HWiNFO's shared memory,
 * and LibreHardwareMonitor / OpenHardwareMonitor's WMI namespace -- so if
 * either tool is running, Tarman shows the real package temperature. */

typedef struct { LUID luid; UINT h; } TmOpenLuid;
typedef struct { UINT h; int type; void* data; UINT size; } TmQueryInfo;
typedef struct {
    UINT32 index;
    ULONGLONG mem_freq, max_mem_freq, max_mem_freq_oc, mem_bw, pcie_bw;
    ULONG fan_rpm, power, temperature;
    UCHAR power_override;
} TmAdapterPerf;
typedef LONG (APIENTRY *PFN_KmtOpen)(TmOpenLuid*);
typedef LONG (APIENTRY *PFN_KmtQuery)(TmQueryInfo*);
static PFN_KmtOpen  pKmtOpen;
static PFN_KmtQuery pKmtQuery;
static UINT         g_kmt[MAX_GPUS];
static bool         g_kmt_tried[MAX_GPUS];

#pragma pack(push, 1)
typedef struct {
    DWORD sig, ver, rev;
    __int64 poll_time;
    DWORD sensor_off, sensor_size, sensor_n;
    DWORD reading_off, reading_size, reading_n;
} HwiHeader;
typedef struct {
    DWORD type, sensor_index, reading_id;
    char  label_orig[128], label_user[128], unit[16];
    double value, vmin, vmax, vavg;
} HwiReading;
#pragma pack(pop)

static HANDLE         g_hwi_map;
static const uint8_t* g_hwi_view;
static IWbemServices* g_wmi;
static char           g_wmi_name[32];

typedef struct {
    bool     cpu_ok;
    float    cpu;
    char     src[48];
    bool     gpu_ok[MAX_GPUS];
    float    gpu[MAX_GPUS], power[MAX_GPUS];
    uint32_t fan[MAX_GPUS];
    uint64_t memf[MAX_GPUS];
} Temps;

static int cpu_label_rank(const char* s)
{
    if (strstr(s, "Tctl/Tdie") || strstr(s, "CPU Package") || !strcmp(s, "Package")) return 0;
    if (strstr(s, "Tctl") || strstr(s, "Tdie") || strstr(s, "CPU (")) return 1;
    if (strstr(s, "Core Average") || strstr(s, "Core Max")) return 2;
    if (strstr(s, "CCD")) return 3;
    return 9;
}

static bool hwinfo_cpu_temp(float* out)
{
    if (!g_hwi_view) {
        g_hwi_map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
        if (!g_hwi_map) return false;
        g_hwi_view = MapViewOfFile(g_hwi_map, FILE_MAP_READ, 0, 0, 0);
        if (!g_hwi_view) { CloseHandle(g_hwi_map); g_hwi_map = NULL; return false; }
    }
    const HwiHeader* h = (const HwiHeader*)g_hwi_view;
    if (h->sig != 0x53695748 /* "HWiS" */) {
        UnmapViewOfFile(g_hwi_view);
        CloseHandle(g_hwi_map);
        g_hwi_view = NULL;
        g_hwi_map = NULL;
        return false;
    }
    int best = 99;
    for (DWORD i = 0; i < h->reading_n && i < 4096; ++i) {
        const HwiReading* r = (const HwiReading*)(g_hwi_view + h->reading_off + (size_t)i * h->reading_size);
        if (r->type != 1 /* temperature */) continue;
        int rank = cpu_label_rank(r->label_orig);
        if (rank < best && (strstr(r->label_orig, "CPU") || rank <= 1)) { best = rank; *out = (float)r->value; }
    }
    return best < 99;
}

static bool wmi_connect(void)
{
    IWbemLocator* loc = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void**)&loc)))
        return false;
    static const wchar_t* const ns[] = { L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor" };
    static const char* const names[] = { "LibreHardwareMonitor", "OpenHardwareMonitor" };
    for (int i = 0; i < 2 && !g_wmi; ++i) {
        BSTR b = SysAllocString(ns[i]);
        if (SUCCEEDED(IWbemLocator_ConnectServer(loc, b, NULL, NULL, NULL, 0, NULL, NULL, &g_wmi)) && g_wmi) {
            CoSetProxyBlanket((IUnknown*)g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
            snprintf(g_wmi_name, sizeof g_wmi_name, "%s", names[i]);
        }
        SysFreeString(b);
    }
    IWbemLocator_Release(loc);
    return g_wmi != NULL;
}

static bool wmi_cpu_temp(float* out)
{
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(L"SELECT Name,Value,Identifier FROM Sensor WHERE SensorType='Temperature'");
    IEnumWbemClassObject* en = NULL;
    HRESULT hr = IWbemServices_ExecQuery(g_wmi, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en) {
        /* the tool exited: drop the connection and look again later */
        IWbemServices_Release(g_wmi);
        g_wmi = NULL;
        return false;
    }
    int best = 99;
    for (;;) {
        IWbemClassObject* o = NULL;
        ULONG got = 0;
        if (IEnumWbemClassObject_Next(en, 1500, 1, &o, &got) != S_OK || !got) break;
        VARIANT vn, vv, vi;
        VariantInit(&vn);
        VariantInit(&vv);
        VariantInit(&vi);
        IWbemClassObject_Get(o, L"Name", 0, &vn, NULL, NULL);
        IWbemClassObject_Get(o, L"Value", 0, &vv, NULL, NULL);
        IWbemClassObject_Get(o, L"Identifier", 0, &vi, NULL, NULL);
        if (vn.vt == VT_BSTR && vi.vt == VT_BSTR && wcsstr(vi.bstrVal, L"cpu/") &&
            SUCCEEDED(VariantChangeType(&vv, &vv, 0, VT_R8))) {
            char name[96];
            w2u(vn.bstrVal, name, sizeof name);
            int rank = cpu_label_rank(name);
            if (rank == 9 && strstr(name, "Core")) rank = 4;
            if (rank < best) { best = rank; *out = (float)vv.dblVal; }
        }
        VariantClear(&vn);
        VariantClear(&vv);
        VariantClear(&vi);
        IWbemClassObject_Release(o);
    }
    IEnumWbemClassObject_Release(en);
    return best < 99;
}

static void gather_temps(const SysState* st, Temps* t, uint64_t tick)
{
    static Temps last;
    memset(t, 0, sizeof *t);
    for (int i = 0; i < st->ngpu && pKmtOpen && pKmtQuery; ++i) {
        if (!g_kmt_tried[i]) {
            g_kmt_tried[i] = true;
            TmOpenLuid o = { { st->gpu[i].luid_lo, st->gpu[i].luid_hi }, 0 };
            if (pKmtOpen(&o) == 0) g_kmt[i] = o.h;
        }
        if (!g_kmt[i]) continue;
        TmAdapterPerf p;
        memset(&p, 0, sizeof p);
        TmQueryInfo q = { g_kmt[i], 62 /* KMTQAITYPE_ADAPTERPERFDATA */, &p, sizeof p };
        if (pKmtQuery(&q) == 0) {
            t->gpu_ok[i] = p.temperature > 0;
            t->gpu[i] = p.temperature / 10.0f;
            t->fan[i] = p.fan_rpm;
            t->power[i] = p.power / 10.0f;
            t->memf[i] = p.mem_freq;
        }
    }

    double best = 0;
    if (c_thermal) {
        DWORD n;
        PDH_FMT_COUNTERVALUE_ITEM_W* it = pdh_array(c_thermal, PDH_FMT_DOUBLE, &n);
        for (DWORD i = 0; i < n; ++i) {
            double c = it[i].FmtValue.doubleValue / 10.0 - 273.15;   /* tenths of a kelvin */
            if (c > best && c < 150) best = c;
        }
        free(it);
    }
    float v = 0;
    if (best > 0) {
        t->cpu_ok = true;
        t->cpu = (float)best;
        snprintf(t->src, sizeof t->src, "ACPI thermal zone");
    } else if (hwinfo_cpu_temp(&v)) {
        t->cpu_ok = true;
        t->cpu = v;
        snprintf(t->src, sizeof t->src, "HWiNFO");
    } else {
        /* WMI is slower: poll every other tick, look for the tool every 30 s */
        if (!g_wmi && tick % 30 == 0) wmi_connect();
        if (g_wmi && tick % 2 == 0 && wmi_cpu_temp(&v)) {
            t->cpu_ok = true;
            t->cpu = v;
            snprintf(t->src, sizeof t->src, "%s", g_wmi_name);
        } else if (g_wmi && last.cpu_ok) {
            t->cpu_ok = true;
            t->cpu = last.cpu;
            snprintf(t->src, sizeof t->src, "%s", last.src);
        }
    }
    last = *t;
}

/* ---- per-tick scratch ------------------------------------------------------ */

typedef struct {
    uint32_t pid, ppid, threads, handles, session, page_faults, hard_faults;
    int32_t  base_prio;
    uint64_t create, cpu, ws_private, ws, ws_peak, commit, virt, paged, nonpaged, io_r, io_w, io_o;
    bool     suspended;
    char     name[64];
} RawProc;

typedef struct { uint32_t pid; float util; uint64_t mem; } PidGpu;
typedef struct { uint32_t pid; char title[128]; } AppWin;

typedef struct {
    int    index;
    char   letters[32];
    float  active, read, write, resp_ms, queue;
} RawDisk;

typedef struct {
    RawProc* procs; int nproc, proc_cap;
    TmCpuPerf cores[MAX_CORES]; int ncores;
    RawDisk disks[MAX_DISKS]; int ndisk;
    double perf;
    uint64_t standby, modified, freezero;
    float gpu_util[MAX_GPUS], gpu_eng[MAX_GPUS][GPU_ENGINES];
    uint64_t gpu_ded[MAX_GPUS], gpu_shared[MAX_GPUS];
    PidGpu* pidgpu; int npidgpu, pidgpu_cap;
    AppWin* apps; int napp, app_cap;
    MIB_IF_TABLE2* ift;
    MEMORYSTATUSEX ms;
    PERFORMANCE_INFORMATION pi;
    Temps temps;
} Scratch;

static Scratch S;

static void pidgpu_add(uint32_t pid, float util, uint64_t mem)
{
    for (int i = 0; i < S.npidgpu; ++i)
        if (S.pidgpu[i].pid == pid) {
            if (util > S.pidgpu[i].util) S.pidgpu[i].util = util;
            S.pidgpu[i].mem += mem;
            return;
        }
    if (S.npidgpu == S.pidgpu_cap) {
        S.pidgpu_cap = S.pidgpu_cap ? S.pidgpu_cap * 2 : 128;
        S.pidgpu = realloc(S.pidgpu, sizeof *S.pidgpu * S.pidgpu_cap);
    }
    S.pidgpu[S.npidgpu++] = (PidGpu){ pid, util, mem };
}

/* ---- gatherers (no lock held) ----------------------------------------------- */

static uint8_t* g_spi;
static ULONG    g_spi_cap;

static void gather_processes(void)
{
    S.nproc = 0;
    for (;;) {
        ULONG need = 0;
        LONG r = pNtQSI(5 /* SystemProcessInformation */, g_spi, g_spi_cap, &need);
        if (r == STATUS_INFO_LENGTH_MISMATCH) {
            g_spi_cap = need + 128 * 1024;
            g_spi = realloc(g_spi, g_spi_cap);
            continue;
        }
        if (r < 0) return;
        break;
    }
    TmProcInfo* p = (TmProcInfo*)g_spi;
    for (;;) {
        if (S.nproc == S.proc_cap) {
            S.proc_cap = S.proc_cap ? S.proc_cap * 2 : 512;
            S.procs = realloc(S.procs, sizeof *S.procs * S.proc_cap);
        }
        RawProc* r = &S.procs[S.nproc++];
        r->pid        = (uint32_t)(uintptr_t)p->UniqueProcessId;
        r->ppid       = (uint32_t)(uintptr_t)p->InheritedFromUniqueProcessId;
        r->threads    = p->NumberOfThreads;
        r->handles    = p->HandleCount;
        r->session    = p->SessionId;
        r->page_faults= p->PageFaultCount;
        r->hard_faults= p->HardFaultCount;
        r->base_prio  = p->BasePriority;
        r->create     = (uint64_t)p->CreateTime.QuadPart;
        r->cpu        = (uint64_t)p->UserTime.QuadPart + (uint64_t)p->KernelTime.QuadPart;
        r->ws_private = (uint64_t)p->WorkingSetPrivateSize.QuadPart;
        r->ws         = p->WorkingSetSize;
        r->ws_peak    = p->PeakWorkingSetSize;
        r->commit     = p->PagefileUsage;
        r->virt       = p->VirtualSize;
        r->paged      = p->QuotaPagedPoolUsage;
        r->nonpaged   = p->QuotaNonPagedPoolUsage;
        r->io_r       = (uint64_t)p->ReadTransferCount.QuadPart;
        r->io_w       = (uint64_t)p->WriteTransferCount.QuadPart;
        r->io_o       = (uint64_t)p->OtherTransferCount.QuadPart;
        if (p->ImageName.Buffer)
            w2u_len(p->ImageName.Buffer, p->ImageName.Length / 2, r->name, sizeof r->name);
        else
            snprintf(r->name, sizeof r->name, r->pid ? "System" : "System Idle Process");
        /* A process is "Suspended" when every thread waits with reason Suspended. */
        TmThreadInfo* t = (TmThreadInfo*)(p + 1);
        r->suspended = p->NumberOfThreads > 0;
        for (ULONG i = 0; i < p->NumberOfThreads; ++i)
            if (!(t[i].ThreadState == 5 && t[i].WaitReason == 5)) { r->suspended = false; break; }
        if (!p->NextEntryOffset) break;
        p = (TmProcInfo*)((uint8_t*)p + p->NextEntryOffset);
    }
}

static void gather_cores(void)
{
    ULONG got = 0;
    S.ncores = 0;
    if (pNtQSI(8 /* SystemProcessorPerformanceInformation */, S.cores, sizeof S.cores, &got) >= 0)
        S.ncores = (int)(got / sizeof(TmCpuPerf));
}

static void parse_disk_instance(const wchar_t* inst, RawDisk* d)
{
    d->index = _wtoi(inst);
    const wchar_t* sp = wcschr(inst, L' ');
    w2u(sp ? sp + 1 : L"", d->letters, sizeof d->letters);
}

static RawDisk* raw_disk(const wchar_t* inst)
{
    if (!wcscmp(inst, L"_Total")) return NULL;
    int idx = _wtoi(inst);
    for (int i = 0; i < S.ndisk; ++i) if (S.disks[i].index == idx) return &S.disks[i];
    if (S.ndisk >= MAX_DISKS) return NULL;
    RawDisk* d = &S.disks[S.ndisk++];
    memset(d, 0, sizeof *d);
    parse_disk_instance(inst, d);
    return d;
}

static void gather_pdh(const SysState* st)
{
    S.ndisk = 0;
    S.npidgpu = 0;
    memset(S.gpu_util, 0, sizeof S.gpu_util);
    memset(S.gpu_eng, 0, sizeof S.gpu_eng);
    memset(S.gpu_ded, 0, sizeof S.gpu_ded);
    memset(S.gpu_shared, 0, sizeof S.gpu_shared);
    if (!g_pdh) return;
    PdhCollectQueryData(g_pdh);

    S.perf     = pdh_value(c_perf);
    S.standby  = (uint64_t)(pdh_value(c_sb_res) + pdh_value(c_sb_norm) + pdh_value(c_sb_core));
    S.modified = (uint64_t)pdh_value(c_modified);
    S.freezero = (uint64_t)pdh_value(c_freezero);

    const DWORD F = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    PDH_HCOUNTER dk[5] = { c_dk_idle, c_dk_read, c_dk_write, c_dk_sec, c_dk_queue };
    for (int k = 0; k < 5; ++k) {
        DWORD n;
        PDH_FMT_COUNTERVALUE_ITEM_W* it = pdh_array(dk[k], F, &n);
        for (DWORD i = 0; i < n; ++i) {
            RawDisk* d = raw_disk(it[i].szName);
            if (!d) continue;
            double v = it[i].FmtValue.doubleValue;
            switch (k) {
            case 0: d->active = (float)(100.0 - (v > 100 ? 100 : v)); if (d->active < 0) d->active = 0; break;
            case 1: d->read = (float)v; break;
            case 2: d->write = (float)v; break;
            case 3: d->resp_ms = (float)(v * 1000.0); break;
            case 4: d->queue = (float)v; break;
            }
        }
        free(it);
    }

    /* GPU engines: an instance per (process, engine). An engine's load is the
     * sum over processes; an adapter's load is its busiest engine -- the same
     * rule Task Manager uses. */
    typedef struct { int gpu; int phys, eng, kind; float sum; } EngAgg;
    static EngAgg agg[512];
    int nagg = 0;
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W* it = pdh_array(c_gpu_eng, F, &n);
    for (DWORD i = 0; i < n; ++i) {
        const wchar_t* s = it[i].szName;
        int32_t hi; uint32_t lo;
        if (!parse_luid(s, &hi, &lo)) continue;
        int g = gpu_find(st, hi, lo);
        if (g < 0) continue;
        float v = (float)it[i].FmtValue.doubleValue;
        const wchar_t* pp = wcsstr(s, L"phys_");
        const wchar_t* pe = wcsstr(s, L"eng_");
        const wchar_t* pt = wcsstr(s, L"engtype_");
        int phys = pp ? _wtoi(pp + 5) : 0, eng = pe ? _wtoi(pe + 4) : 0, kind = -1;
        if (pt) {
            pt += 8;
            if (!wcscmp(pt, L"3D")) kind = 0;
            else if (!wcscmp(pt, L"Copy")) kind = 1;
            else if (!wcscmp(pt, L"VideoEncode")) kind = 2;
            else if (!wcscmp(pt, L"VideoDecode")) kind = 3;
        }
        int a = 0;
        for (; a < nagg; ++a)
            if (agg[a].gpu == g && agg[a].phys == phys && agg[a].eng == eng) break;
        if (a == nagg) {
            if (nagg == 512) continue;
            agg[nagg++] = (EngAgg){ g, phys, eng, kind, 0 };
        }
        agg[a].sum += v;
        const wchar_t* ppid = wcsstr(s, L"pid_");
        if (ppid && v > 0) pidgpu_add((uint32_t)_wtoi(ppid + 4), v, 0);
    }
    free(it);
    for (int a = 0; a < nagg; ++a) {
        float v = agg[a].sum > 100 ? 100 : agg[a].sum;
        if (v > S.gpu_util[agg[a].gpu]) S.gpu_util[agg[a].gpu] = v;
        if (agg[a].kind >= 0 && v > S.gpu_eng[agg[a].gpu][agg[a].kind]) S.gpu_eng[agg[a].gpu][agg[a].kind] = v;
    }

    PDH_HCOUNTER gm[2] = { c_gpu_ded, c_gpu_shared };
    for (int k = 0; k < 2; ++k) {
        it = pdh_array(gm[k], PDH_FMT_LARGE, &n);
        for (DWORD i = 0; i < n; ++i) {
            int32_t hi; uint32_t lo;
            if (!parse_luid(it[i].szName, &hi, &lo)) continue;
            int g = gpu_find(st, hi, lo);
            if (g < 0) continue;
            uint64_t v = (uint64_t)it[i].FmtValue.largeValue;
            if (k == 0) S.gpu_ded[g] += v; else S.gpu_shared[g] += v;
        }
        free(it);
    }
    it = pdh_array(c_gpu_pmem, PDH_FMT_LARGE, &n);
    for (DWORD i = 0; i < n; ++i) {
        const wchar_t* ppid = wcsstr(it[i].szName, L"pid_");
        if (ppid && it[i].FmtValue.largeValue > 0)
            pidgpu_add((uint32_t)_wtoi(ppid + 4), 0, (uint64_t)it[i].FmtValue.largeValue);
    }
    free(it);
}

static BOOL CALLBACK find_core_window(HWND h, LPARAM lp)
{
    DWORD* io = (DWORD*)lp;     /* io[0] = frame host pid in, io[1] = app pid out */
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid && pid != io[0]) { io[1] = pid; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK enum_app_window(HWND h, LPARAM lp)
{
    (void)lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(h) <= 0) return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"ApplicationFrameWindow")) {
        /* UWP: the frame belongs to ApplicationFrameHost; the app is the
         * owner of the CoreWindow child. */
        DWORD io[2] = { pid, 0 };
        EnumChildWindows(h, find_core_window, (LPARAM)io);
        if (io[1]) pid = io[1];
    }
    for (int i = 0; i < S.napp; ++i) if (S.apps[i].pid == pid) return TRUE;
    if (S.napp == S.app_cap) {
        S.app_cap = S.app_cap ? S.app_cap * 2 : 64;
        S.apps = realloc(S.apps, sizeof *S.apps * S.app_cap);
    }
    AppWin* a = &S.apps[S.napp++];
    a->pid = pid;
    wchar_t t[128];
    GetWindowTextW(h, t, 128);
    w2u(t, a->title, sizeof a->title);
    return TRUE;
}

static void gather_apps(void)
{
    S.napp = 0;
    EnumWindows(enum_app_window, 0);
}

static void gather_net(void)
{
    if (S.ift) { FreeMibTable(S.ift); S.ift = NULL; }
    GetIfTable2(&S.ift);
}

/* ---- slow lists (built off-lock, swapped in under it) ----------------------- */

static void ip_string(int af, const void* addr, char* out, int n)
{
    if (!inet_ntop(af, addr, out, n)) snprintf(out, n, "?");
}

static ConnInfo* gather_conns(int* count)
{
    int cap = 256, cnt = 0;
    ConnInfo* out = malloc(sizeof *out * cap);
    if (!out) { *count = 0; return NULL; }
#define PUSH() do { if (cnt == cap) { cap *= 2; out = realloc(out, sizeof *out * cap); } } while (0)
    for (int fam = 0; fam < 2; ++fam) {
        ULONG af = fam ? AF_INET6 : AF_INET, sz = 0;
        GetExtendedTcpTable(NULL, &sz, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0);
        uint8_t* buf = malloc(sz + 4096);
        sz += 4096;
        if (buf && GetExtendedTcpTable(buf, &sz, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            if (!fam) {
                MIB_TCPTABLE_OWNER_PID* t = (MIB_TCPTABLE_OWNER_PID*)buf;
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    PUSH();
                    ConnInfo* c = &out[cnt++];
                    memset(c, 0, sizeof *c);
                    MIB_TCPROW_OWNER_PID* r = &t->table[i];
                    c->pid = r->dwOwningPid; c->state = (int)r->dwState;
                    ip_string(AF_INET, &r->dwLocalAddr, c->local, sizeof c->local);
                    ip_string(AF_INET, &r->dwRemoteAddr, c->remote, sizeof c->remote);
                    c->lport = ntohs((u_short)r->dwLocalPort);
                    c->rport = ntohs((u_short)r->dwRemotePort);
                }
            } else {
                MIB_TCP6TABLE_OWNER_PID* t = (MIB_TCP6TABLE_OWNER_PID*)buf;
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    PUSH();
                    ConnInfo* c = &out[cnt++];
                    memset(c, 0, sizeof *c);
                    MIB_TCP6ROW_OWNER_PID* r = &t->table[i];
                    c->v6 = 1; c->pid = r->dwOwningPid; c->state = (int)r->dwState;
                    ip_string(AF_INET6, r->ucLocalAddr, c->local, sizeof c->local);
                    ip_string(AF_INET6, r->ucRemoteAddr, c->remote, sizeof c->remote);
                    c->lport = ntohs((u_short)r->dwLocalPort);
                    c->rport = ntohs((u_short)r->dwRemotePort);
                }
            }
        }
        free(buf);

        sz = 0;
        GetExtendedUdpTable(NULL, &sz, FALSE, af, UDP_TABLE_OWNER_PID, 0);
        sz += 4096;
        buf = malloc(sz);
        if (buf && GetExtendedUdpTable(buf, &sz, FALSE, af, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            if (!fam) {
                MIB_UDPTABLE_OWNER_PID* t = (MIB_UDPTABLE_OWNER_PID*)buf;
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    PUSH();
                    ConnInfo* c = &out[cnt++];
                    memset(c, 0, sizeof *c);
                    c->udp = 1; c->pid = t->table[i].dwOwningPid;
                    ip_string(AF_INET, &t->table[i].dwLocalAddr, c->local, sizeof c->local);
                    c->lport = ntohs((u_short)t->table[i].dwLocalPort);
                }
            } else {
                MIB_UDP6TABLE_OWNER_PID* t = (MIB_UDP6TABLE_OWNER_PID*)buf;
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    PUSH();
                    ConnInfo* c = &out[cnt++];
                    memset(c, 0, sizeof *c);
                    c->udp = 1; c->v6 = 1; c->pid = t->table[i].dwOwningPid;
                    ip_string(AF_INET6, t->table[i].ucLocalAddr, c->local, sizeof c->local);
                    c->lport = ntohs((u_short)t->table[i].dwLocalPort);
                }
            }
        }
        free(buf);
    }
#undef PUSH
    *count = cnt;
    return out;
}

typedef struct { char name[96]; int start; bool delayed; } SvcCfg;
static SvcCfg* g_svccfg;
static int     g_nsvccfg;

static ServiceInfo* gather_services(int* count, bool refresh_config)
{
    *count = 0;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return NULL;
    DWORD need = 0, n = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          NULL, 0, &need, &n, &resume, NULL);
    uint8_t* buf = malloc(need + 4096);
    ServiceInfo* out = NULL;
    resume = 0;
    if (buf && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                     buf, need + 4096, &need, &n, &resume, NULL)) {
        ENUM_SERVICE_STATUS_PROCESSW* e = (ENUM_SERVICE_STATUS_PROCESSW*)buf;
        out = calloc(n ? n : 1, sizeof *out);
        if (refresh_config) {
            free(g_svccfg);
            g_svccfg = calloc(n ? n : 1, sizeof *g_svccfg);
            g_nsvccfg = 0;
        }
        for (DWORD i = 0; out && i < n; ++i) {
            ServiceInfo* s = &out[i];
            w2u(e[i].lpServiceName, s->name, sizeof s->name);
            w2u(e[i].lpDisplayName, s->display, sizeof s->display);
            s->pid   = e[i].ServiceStatusProcess.dwProcessId;
            s->state = (int)e[i].ServiceStatusProcess.dwCurrentState;
            s->start_type = -1;
            if (refresh_config && g_svccfg) {
                SC_HANDLE h = OpenServiceW(scm, e[i].lpServiceName, SERVICE_QUERY_CONFIG);
                SvcCfg* c = &g_svccfg[g_nsvccfg++];
                snprintf(c->name, sizeof c->name, "%s", s->name);
                c->start = -1;
                if (h) {
                    uint8_t cb[8192];
                    DWORD got;
                    if (QueryServiceConfigW(h, (QUERY_SERVICE_CONFIGW*)cb, sizeof cb, &got))
                        c->start = (int)((QUERY_SERVICE_CONFIGW*)cb)->dwStartType;
                    SERVICE_DELAYED_AUTO_START_INFO di = { 0 };
                    if (c->start == SERVICE_AUTO_START &&
                        QueryServiceConfig2W(h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (LPBYTE)&di, sizeof di, &got))
                        c->delayed = di.fDelayedAutostart != 0;
                    CloseServiceHandle(h);
                }
            }
            for (int k = 0; k < g_nsvccfg; ++k)
                if (!strcmp(g_svccfg[k].name, s->name)) {
                    s->start_type = g_svccfg[k].start;
                    s->delayed    = g_svccfg[k].delayed;
                    break;
                }
        }
        *count = out ? (int)n : 0;
    }
    free(buf);
    CloseServiceHandle(scm);
    return out;
}

/* ---- startup entries ---------------------------------------------------------- */

static const wchar_t* const APPROVED_KEY[] = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
};

static HKEY source_root(int src)
{
    return (src == STARTUP_HKCU_RUN || src == STARTUP_USER_FOLDER) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

static bool startup_enabled(int src, const wchar_t* name)
{
    uint8_t data[16];
    DWORD sz = sizeof data;
    if (RegGetValueW(source_root(src), APPROVED_KEY[src], name, RRF_RT_REG_BINARY, NULL, data, &sz) != ERROR_SUCCESS)
        return true;
    return sz == 0 || (data[0] & 1) == 0;
}

/* Command line -> executable path: quoted first token, else up to ".exe". */
static void command_target(const char* cmd, char* out, int n)
{
    wchar_t wc[600], ex[600];
    u2w(cmd, wc, 600);
    ExpandEnvironmentStringsW(wc, ex, 600);
    wchar_t* p = ex;
    while (*p == L' ') ++p;
    wchar_t* end;
    if (*p == L'"') { ++p; end = wcschr(p, L'"'); }
    else {
        end = NULL;
        for (wchar_t* q = p; *q; ++q)
            if (!_wcsnicmp(q, L".exe", 4)) { end = q + 4; break; }
        if (!end) end = wcschr(p, L' ');
    }
    if (end) *end = 0;
    w2u(p, out, n);
}

static void file_version_strings(const wchar_t* path, char* desc, int dn, char* company, int cn);

static void startup_push(StartupItem** arr, int* cnt, int* cap, int src, const wchar_t* name,
                         const char* command)
{
    if (*cnt == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *arr = realloc(*arr, sizeof **arr * *cap);
    }
    StartupItem* it = &(*arr)[(*cnt)++];
    memset(it, 0, sizeof *it);
    it->source = src;
    w2u(name, it->name, sizeof it->name);
    snprintf(it->command, sizeof it->command, "%s", command);
    it->enabled = startup_enabled(src, name);
    command_target(command, it->target, sizeof it->target);
    wchar_t wt[300];
    u2w(it->target, wt, 300);
    char desc[128] = "";
    file_version_strings(wt, desc, sizeof desc, it->publisher, sizeof it->publisher);
    snprintf(it->display, sizeof it->display, "%s", desc[0] ? desc : it->name);
    if (src >= STARTUP_USER_FOLDER) {
        char* dot = strrchr(it->display, '.');
        if (dot && !desc[0]) *dot = 0;
    }
}

static void resolve_lnk(const wchar_t* lnk, char* out, int n)
{
    out[0] = 0;
    IShellLinkW* sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&sl)))
        return;
    IPersistFile* pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void**)&pf))) {
        if (SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ))) {
            wchar_t target[MAX_PATH], args[512];
            if (SUCCEEDED(IShellLinkW_GetPath(sl, target, MAX_PATH, NULL, 0)) && target[0]) {
                args[0] = 0;
                IShellLinkW_GetArguments(sl, args, 512);
                char t8[300], a8[300];
                w2u(target, t8, sizeof t8);
                w2u(args, a8, sizeof a8);
                snprintf(out, n, a8[0] ? "\"%s\" %s" : "%s", t8, a8);
            }
        }
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

static StartupItem* gather_startup(int* count)
{
    StartupItem* arr = NULL;
    int cnt = 0, cap = 0;
    const wchar_t* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    struct { HKEY root; REGSAM sam; int src; } keys[] = {
        { HKEY_CURRENT_USER,  KEY_WOW64_64KEY, STARTUP_HKCU_RUN },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, STARTUP_HKLM_RUN },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, STARTUP_HKLM_RUN32 },
    };
    for (int k = 0; k < 3; ++k) {
        HKEY h;
        if (RegOpenKeyExW(keys[k].root, run, 0, KEY_READ | keys[k].sam, &h) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[256], data[1024];
            DWORD nn = 256, dn = sizeof data, type = 0;
            LONG r = RegEnumValueW(h, i, name, &nn, NULL, &type, (LPBYTE)data, &dn);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
            data[sizeof data / sizeof data[0] - 1] = 0;
            char cmd[600];
            w2u(data, cmd, sizeof cmd);
            startup_push(&arr, &cnt, &cap, keys[k].src, name, cmd);
        }
        RegCloseKey(h);
    }
    int csidl[2] = { CSIDL_STARTUP, CSIDL_COMMON_STARTUP };
    for (int k = 0; k < 2; ++k) {
        wchar_t dir[MAX_PATH], pat[MAX_PATH + 4];
        if (FAILED(SHGetFolderPathW(NULL, csidl[k], NULL, 0, dir))) continue;
        _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", dir);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
            wchar_t full[MAX_PATH * 2];
            _snwprintf(full, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            char cmd[600];
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (ext && !_wcsicmp(ext, L".lnk")) resolve_lnk(full, cmd, sizeof cmd);
            else cmd[0] = 0;
            if (!cmd[0]) w2u(full, cmd, sizeof cmd);
            startup_push(&arr, &cnt, &cap, STARTUP_USER_FOLDER + k, fd.cFileName, cmd);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    *count = cnt;
    return arr;
}

static void gather_sessions(SessionInfo* out, int* count)
{
    *count = 0;
    WTS_SESSION_INFOW* info = NULL;
    DWORD n = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &n)) return;
    static const char* const states[] = { "Active", "Connected", "Connect query", "Shadow",
                                          "Disconnected", "Idle", "Listening", "Reset", "Down", "Init" };
    for (DWORD i = 0; i < n && *count < MAX_SESSIONS; ++i) {
        wchar_t* user = NULL;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, info[i].SessionId, WTSUserName, &user, &bytes))
            continue;
        if (user && user[0]) {
            SessionInfo* s = &out[(*count)++];
            s->id = info[i].SessionId;
            w2u(user, s->user, sizeof s->user);
            w2u(info[i].pWinStationName, s->station, sizeof s->station);
            int st = info[i].State;
            snprintf(s->state, sizeof s->state, "%s", st >= 0 && st < 10 ? states[st] : "?");
        }
        WTSFreeMemory(user);
    }
    WTSFreeMemory(info);
}

static void gather_volumes(VolumeInfo* out, int* count)
{
    *count = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && *count < MAX_VOLUMES; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) continue;
        VolumeInfo* v = &out[*count];
        memset(v, 0, sizeof *v);
        v->kind = t == DRIVE_FIXED ? 0 : t == DRIVE_REMOVABLE ? 1 : t == DRIVE_REMOTE ? 2 : t == DRIVE_CDROM ? 3 : 4;
        w2u(root, v->root, sizeof v->root);
        if (t == DRIVE_CDROM || t == DRIVE_REMOTE) {
            /* Never block the sampler on a sleeping network share or empty tray. */
            UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);
            SetErrorMode(old | SEM_FAILCRITICALERRORS);
        }
        wchar_t label[64] = L"", fs[16] = L"";
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) {
            GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 16);
            ULARGE_INTEGER avail, total, freeb;
            if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) {
                v->total = total.QuadPart;
                v->free  = freeb.QuadPart;
            }
        }
        w2u(label, v->label, sizeof v->label);
        w2u(fs, v->fs, sizeof v->fs);
        (*count)++;
    }
}

static void fill_adapter_addresses(SysState* st)
{
    ULONG sz = 32 * 1024;
    IP_ADAPTER_ADDRESSES* aa = malloc(sz);
    if (!aa) return;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, aa, &sz);
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(aa);
        aa = malloc(sz);
        if (!aa) return;
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, aa, &sz);
    }
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
            for (int i = 0; i < st->nnet; ++i) {
                NetInfo* n = &st->net[i];
                if (n->luid != a->Luid.Value) continue;
                n->ipv4[0] = n->ipv6[0] = 0;
                for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                    SOCKADDR* sa = u->Address.lpSockaddr;
                    if (sa->sa_family == AF_INET && !n->ipv4[0])
                        ip_string(AF_INET, &((SOCKADDR_IN*)sa)->sin_addr, n->ipv4, sizeof n->ipv4);
                    if (sa->sa_family == AF_INET6) {
                        IN6_ADDR* a6 = &((SOCKADDR_IN6*)sa)->sin6_addr;
                        bool link_local = a6->s6_addr[0] == 0xfe && (a6->s6_addr[1] & 0xc0) == 0x80;
                        if (!n->ipv6[0] || (!link_local && strncmp(n->ipv6, "fe80", 4) == 0))
                            ip_string(AF_INET6, a6, n->ipv6, sizeof n->ipv6);
                    }
                }
                w2u(a->DnsSuffix, n->dns, sizeof n->dns);
            }
        }
    }
    free(aa);
}

/* ---- per-process metadata --------------------------------------------------- */

static void file_version_strings(const wchar_t* path, char* desc, int dn, char* company, int cn)
{
    if (dn) desc[0] = 0;
    if (cn) company[0] = 0;
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return;
    void* data = malloc(sz);
    if (!data) return;
    if (GetFileVersionInfoW(path, 0, sz, data)) {
        struct { WORD lang, cp; }* tr = NULL;
        UINT trn = 0;
        WORD lang = 0x0409, cp = 0x04B0;
        if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void**)&tr, &trn) && trn >= 4) {
            lang = tr[0].lang;
            cp   = tr[0].cp;
        }
        wchar_t q[96];
        wchar_t* v = NULL;
        UINT vn = 0;
        _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\FileDescription", lang, cp);
        if (dn && VerQueryValueW(data, q, (void**)&v, &vn) && vn > 1) w2u(v, desc, dn);
        _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\CompanyName", lang, cp);
        if (cn && VerQueryValueW(data, q, (void**)&v, &vn) && vn > 1) w2u(v, company, cn);
    }
    free(data);
    if (dn) trim(desc);
    if (cn) trim(company);
}

static int icon_add(uint8_t* rgba, int w, int h)
{
    sys_lock();
    if (g_st.nicon == g_st.icon_cap) {
        g_st.icon_cap = g_st.icon_cap ? g_st.icon_cap * 2 : 128;
        g_st.icons = realloc(g_st.icons, sizeof *g_st.icons * g_st.icon_cap);
    }
    int id = g_st.nicon++;
    g_st.icons[id] = (IconEntry){ 0, w, h, rgba };
    sys_unlock();
    return id;
}

static int icon_from_file(const wchar_t* path)
{
    HICON hi = NULL;
    if (ExtractIconExW(path, 0, &hi, NULL, 1) == 0 || !hi) return -1;
    ICONINFO ii;
    int id = -1;
    if (GetIconInfo(hi, &ii)) {
        BITMAP bm;
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm)) {
            int w = bm.bmWidth, h = bm.bmHeight;
            uint8_t* px = malloc((size_t)w * h * 4);
            BITMAPINFO bi = { 0 };
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            HDC dc = GetDC(NULL);
            if (px && GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS)) {
                bool has_alpha = false;
                for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3]) { has_alpha = true; break; }
                if (!has_alpha && ii.hbmMask) {
                    uint8_t* mk = malloc((size_t)w * h * 4);
                    if (mk && GetDIBits(dc, ii.hbmMask, 0, h, mk, &bi, DIB_RGB_COLORS))
                        for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = mk[i * 4] ? 0 : 255;
                    free(mk);
                }
                for (int i = 0; i < w * h; ++i) {
                    uint8_t t = px[i * 4];
                    px[i * 4] = px[i * 4 + 2];
                    px[i * 4 + 2] = t;
                }
                id = icon_add(px, w, h);
                px = NULL;
            }
            free(px);
            ReleaseDC(NULL, dc);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    DestroyIcon(hi);
    return id;
}

/* Path -> (description, company, icon), since a dozen svchosts share one. */
typedef struct { uint32_t hash; char desc[128]; char company[96]; int icon; } PathMeta;
#define PATH_CACHE 2048
static PathMeta g_pathmeta[PATH_CACHE];

static const PathMeta* path_meta(const char* path)
{
    uint32_t h = hash_str(path);
    uint32_t i = h & (PATH_CACHE - 1);
    for (int probe = 0; probe < PATH_CACHE; ++probe, i = (i + 1) & (PATH_CACHE - 1)) {
        if (g_pathmeta[i].hash == h) return &g_pathmeta[i];
        if (!g_pathmeta[i].hash) {
            PathMeta* m = &g_pathmeta[i];
            wchar_t w[300];
            u2w(path, w, 300);
            file_version_strings(w, m->desc, sizeof m->desc, m->company, sizeof m->company);
            m->icon = icon_from_file(w);
            m->hash = h;
            return m;
        }
    }
    return NULL;
}

typedef struct { uint8_t sid[68]; DWORD len; char name[64]; } SidName;
static SidName g_sids[64];
static int     g_nsid;

static void token_user(HANDLE proc, char* out, int n)
{
    out[0] = 0;
    HANDLE tok;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) return;
    uint8_t buf[256];
    DWORD got = 0;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &got)) {
        PSID sid = ((TOKEN_USER*)buf)->User.Sid;
        DWORD len = GetLengthSid(sid);
        for (int i = 0; i < g_nsid; ++i)
            if (g_sids[i].len == len && !memcmp(g_sids[i].sid, sid, len)) {
                snprintf(out, n, "%s", g_sids[i].name);
                CloseHandle(tok);
                return;
            }
        wchar_t name[64], dom[64];
        DWORD nn = 64, dn = 64;
        SID_NAME_USE use;
        if (LookupAccountSidW(NULL, sid, name, &nn, dom, &dn, &use)) w2u(name, out, n);
        if (g_nsid < 64 && len <= sizeof g_sids[0].sid) {
            memcpy(g_sids[g_nsid].sid, sid, len);
            g_sids[g_nsid].len = len;
            snprintf(g_sids[g_nsid].name, sizeof g_sids[g_nsid].name, "%s", out);
            g_nsid++;
        }
    }
    CloseHandle(tok);
}

static void process_cmdline(HANDLE h, char* out, int n)
{
    out[0] = 0;
    if (!pNtQIP) return;
    ULONG sz = 4096, got = 0;
    uint8_t* buf = malloc(sz);
    LONG r = buf ? pNtQIP(h, TM_ProcessCommandLine, buf, sz, &got) : -1;
    if (r == STATUS_INFO_LENGTH_MISMATCH && got > sz && got < (1u << 20)) {
        free(buf);
        sz = got;
        buf = malloc(sz);
        r = buf ? pNtQIP(h, TM_ProcessCommandLine, buf, sz, &got) : -1;
    }
    if (r >= 0 && buf) {
        UNICODE_STRING* us = (UNICODE_STRING*)buf;
        w2u_len(us->Buffer, us->Length / 2, out, n);
    }
    free(buf);
}

typedef struct {
    uint32_t pid;
    uint64_t create;
    char path[300], desc[128], company[96], user[64], cmdline[1024];
    int  icon;
} MetaJob;

static void resolve_meta(MetaJob* j)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, j->pid);
    j->icon = -1;
    if (h) {
        wchar_t w[300];
        DWORD n = 300;
        if (QueryFullProcessImageNameW(h, 0, w, &n)) w2u(w, j->path, sizeof j->path);
        token_user(h, j->user, sizeof j->user);
        process_cmdline(h, j->cmdline, sizeof j->cmdline);
        CloseHandle(h);
    }
    if (j->path[0]) {
        const PathMeta* m = path_meta(j->path);
        if (m) {
            snprintf(j->desc, sizeof j->desc, "%s", m->desc);
            snprintf(j->company, sizeof j->company, "%s", m->company);
            j->icon = m->icon;
        }
    }
    if (!j->user[0] && j->pid <= 4) snprintf(j->user, sizeof j->user, "SYSTEM");
}

/* ---- the tick ------------------------------------------------------------------- */

/* ---- demo mode ----------------------------------------------------------------
 *
 * Replaces the process snapshot, window list and CPU load with a synthetic but
 * plausible machine. System paths are real so Windows' own icons show; user
 * apps live under a fictional C:\Users\demo profile. */

static bool g_demo;
void sys_set_demo(bool on) { g_demo = on; }
bool sys_is_demo(void)     { return g_demo; }

typedef struct {
    const char* name;
    const char* desc;
    const char* path;
    const char* user;
    int         parent;       /* index in DEMO, -1 none */
    float       cpu, wobble;  /* base %, swing %        */
    float       mem_mb;
    float       io_kbps;
    float       gpu;
    const char* title;        /* visible window => app  */
    int         threads;
} DemoDef;

#define SYS32 "C:\\Windows\\System32\\"
#define UAPP  "C:\\Users\\demo\\AppData\\Local\\"
static const DemoDef DEMO[] = {
    /*  0 */ { "System", "System", "", "SYSTEM", -1, 0.4f, 0.4f, 0.1f, 40, 0.8f, NULL, 210 },
    /*  1 */ { "Registry", "Registry", "", "SYSTEM", 0, 0, 0, 38, 0, 0, NULL, 4 },
    /*  2 */ { "smss.exe", "Windows Session Manager", SYS32 "smss.exe", "SYSTEM", 0, 0, 0, 0.4f, 0, 0, NULL, 2 },
    /*  3 */ { "Memory Compression", "Memory Compression", "", "SYSTEM", 0, 0.1f, 0.2f, 412, 0, 0, NULL, 30 },
    /*  4 */ { "csrss.exe", "Client Server Runtime Process", SYS32 "csrss.exe", "SYSTEM", -1, 0.1f, 0.2f, 1.6f, 0, 0, NULL, 13 },
    /*  5 */ { "wininit.exe", "Windows Start-Up Application", SYS32 "wininit.exe", "SYSTEM", -1, 0, 0, 1.1f, 0, 0, NULL, 2 },
    /*  6 */ { "services.exe", "Services and Controller app", SYS32 "services.exe", "SYSTEM", 5, 0.1f, 0.1f, 6.3f, 0, 0, NULL, 9 },
    /*  7 */ { "lsass.exe", "Local Security Authority Process", SYS32 "lsass.exe", "SYSTEM", 5, 0.1f, 0.2f, 9.8f, 2, 0, NULL, 11 },
    /*  8 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "SYSTEM", 6, 0.2f, 0.3f, 24, 4, 0, NULL, 18 },
    /*  9 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "NETWORK SERVICE", 6, 0.1f, 0.2f, 11, 18, 0, NULL, 12 },
    /* 10 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "LOCAL SERVICE", 6, 0, 0.1f, 6.1f, 0, 0, NULL, 7 },
    /* 11 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "SYSTEM", 6, 0.3f, 0.6f, 31, 12, 0, NULL, 24 },
    /* 12 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "demo", 6, 0, 0.1f, 8.4f, 0, 0, NULL, 9 },
    /* 13 */ { "svchost.exe", "Host Process for Windows Services", SYS32 "svchost.exe", "LOCAL SERVICE", 6, 0.1f, 0.1f, 14, 2, 0, NULL, 10 },
    /* 14 */ { "spoolsv.exe", "Spooler SubSystem App", SYS32 "spoolsv.exe", "SYSTEM", 6, 0, 0, 5.2f, 0, 0, NULL, 8 },
    /* 15 */ { "MsMpEng.exe", "Antimalware Service Executable", "C:\\Program Files\\Windows Defender\\MsMpEng.exe", "SYSTEM", 6, 0.6f, 2.5f, 186, 240, 0, NULL, 46 },
    /* 16 */ { "dwm.exe", "Desktop Window Manager", SYS32 "dwm.exe", "DWM-1", 5, 1.2f, 1.4f, 96, 0, 4.5f, NULL, 22 },
    /* 17 */ { "winlogon.exe", "Windows Logon Application", SYS32 "winlogon.exe", "SYSTEM", -1, 0, 0, 3.4f, 0, 0, NULL, 5 },
    /* 18 */ { "explorer.exe", "Windows Explorer", "C:\\Windows\\explorer.exe", "demo", 17, 0.5f, 1.0f, 142, 22, 0.2f, "Documents - File Explorer", 96 },
    /* 19 */ { "sihost.exe", "Shell Infrastructure Host", SYS32 "sihost.exe", "demo", 8, 0, 0.1f, 9.6f, 0, 0, NULL, 12 },
    /* 20 */ { "taskhostw.exe", "Host Process for Windows Tasks", SYS32 "taskhostw.exe", "demo", 8, 0, 0.1f, 6.8f, 0, 0, NULL, 9 },
    /* 21 */ { "ctfmon.exe", "CTF Loader", SYS32 "ctfmon.exe", "demo", 8, 0, 0.1f, 7.2f, 0, 0, NULL, 10 },
    /* 22 */ { "RuntimeBroker.exe", "Runtime Broker", SYS32 "RuntimeBroker.exe", "demo", 8, 0, 0.1f, 8.9f, 0, 0, NULL, 6 },
    /* 23 */ { "SearchHost.exe", "Search", "C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\SearchHost.exe", "demo", 8, 0, 0.1f, 64, 0, 0, NULL, 41 },
    /* 24 */ { "StartMenuExperienceHost.exe", "Start", "C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe", "demo", 8, 0, 0, 48, 0, 0, NULL, 23 },
    /* 25 */ { "audiodg.exe", "Windows Audio Device Graph Isolation", SYS32 "audiodg.exe", "LOCAL SERVICE", 10, 0.4f, 0.3f, 14, 0, 0, NULL, 7 },
    /* 26 */ { "firefox.exe", "Firefox", "C:\\Program Files\\Mozilla Firefox\\firefox.exe", "demo", 18, 2.0f, 4.0f, 412, 140, 1.5f, "Tarman - GitHub - Mozilla Firefox", 98 },
    /* 27 */ { "firefox.exe", "Firefox", "C:\\Program Files\\Mozilla Firefox\\firefox.exe", "demo", 26, 1.2f, 3.0f, 286, 20, 3.0f, NULL, 32 },
    /* 28 */ { "firefox.exe", "Firefox", "C:\\Program Files\\Mozilla Firefox\\firefox.exe", "demo", 26, 0.4f, 1.0f, 164, 8, 0, NULL, 26 },
    /* 29 */ { "firefox.exe", "Firefox", "C:\\Program Files\\Mozilla Firefox\\firefox.exe", "demo", 26, 0.1f, 0.4f, 92, 2, 0, NULL, 22 },
    /* 30 */ { "Code.exe", "Visual Studio Code", UAPP "Programs\\Microsoft VS Code\\Code.exe", "demo", 18, 0.9f, 2.0f, 228, 60, 0.6f, "main.c - tarman - Visual Studio Code", 58 },
    /* 31 */ { "Code.exe", "Visual Studio Code", UAPP "Programs\\Microsoft VS Code\\Code.exe", "demo", 30, 1.4f, 2.5f, 344, 30, 0, NULL, 24 },
    /* 32 */ { "Code.exe", "Visual Studio Code", UAPP "Programs\\Microsoft VS Code\\Code.exe", "demo", 30, 0.2f, 0.4f, 118, 4, 0, NULL, 18 },
    /* 33 */ { "clangd.exe", "clangd language server", UAPP "Programs\\LLVM\\bin\\clangd.exe", "demo", 31, 0.6f, 5.0f, 260, 90, 0, NULL, 14 },
    /* 34 */ { "WindowsTerminal.exe", "Terminal", "C:\\Program Files\\WindowsApps\\Microsoft.WindowsTerminal\\WindowsTerminal.exe", "demo", 18, 0.2f, 0.4f, 72, 0, 0.3f, "Windows PowerShell", 30 },
    /* 35 */ { "pwsh.exe", "PowerShell 7", "C:\\Program Files\\PowerShell\\7\\pwsh.exe", "demo", 34, 0, 0.2f, 88, 0, 0, NULL, 19 },
    /* 36 */ { "Spotify.exe", "Spotify", UAPP "Microsoft\\WindowsApps\\Spotify.exe", "demo", 18, 0.7f, 0.8f, 196, 24, 0.4f, "Spotify Premium", 44 },
    /* 37 */ { "Slack.exe", "Slack", UAPP "slack\\slack.exe", "demo", 18, 0.5f, 1.2f, 310, 12, 0.2f, "general - Northwind - Slack", 40 },
    /* 38 */ { "steam.exe", "Steam", "C:\\Program Files (x86)\\Steam\\steam.exe", "demo", 18, 0.3f, 0.5f, 140, 6, 0, NULL, 88 },
    /* 39 */ { "OneDrive.exe", "Microsoft OneDrive", UAPP "Microsoft\\OneDrive\\OneDrive.exe", "demo", 18, 0.1f, 0.3f, 54, 30, 0, NULL, 28 },
    /* 40 */ { "SecurityHealthSystray.exe", "Windows Security notification icon", SYS32 "SecurityHealthSystray.exe", "demo", 18, 0, 0, 4.1f, 0, 0, NULL, 3 },
    /* 41 */ { "notepad.exe", "Notepad", SYS32 "notepad.exe", "demo", 18, 0, 0.2f, 28, 0, 0, "notes.txt - Notepad", 8 },
    /* 42 */ { "nvcontainer.exe", "NVIDIA Container", "C:\\Program Files\\NVIDIA Corporation\\NvContainer\\nvcontainer.exe", "SYSTEM", 6, 0.1f, 0.2f, 34, 0, 0, NULL, 30 },
    /* 43 */ { "fontdrvhost.exe", "Usermode Font Driver Host", SYS32 "fontdrvhost.exe", "UMFD-1", 5, 0, 0, 4.4f, 0, 0, NULL, 5 },
    /* 44 */ { "tarman.exe", "Tarman - Task manager and resource monitor", NULL, "demo", 18, 0.7f, 0.4f, 72, 0, 0.6f, "Tarman", 13 },
    /* 45 */ { "updater.exe", "Northwind Updater", UAPP "Northwind\\updater.exe", "demo", 6, 6.0f, 6.0f, 46, 2400, 0, NULL, 9 },
};
#define NDEMO ((int)(sizeof DEMO / sizeof DEMO[0]))

typedef struct {
    uint32_t pid;
    uint64_t create;
    uint64_t cpu, io_r, io_w;
    bool     alive;
} DemoState;
static DemoState g_ds[NDEMO];
static double    g_demo_t;
static char      g_self_path[MAX_PATH * 3];

static float demo_noise(int i, double t)
{
    /* cheap deterministic wobble: three sines at unrelated rates per process */
    return (float)(0.5 + 0.25 * sin(t * (0.11 + i * 0.013) + i) + 0.15 * sin(t * (0.53 + i * 0.07) + i * 2.1) +
                   0.10 * sin(t * (1.7 + i * 0.05) + i * 0.7));
}

static void demo_processes(double dt)
{
    S.nproc = 0;
    g_demo_t += dt > 0 ? dt : 1;
    double t = g_demo_t;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t now64 = ft64(now);
    int ncores = S.ncores > 0 ? S.ncores : 8;
    for (int i = 0; i < NDEMO; ++i) {
        DemoState* d = &g_ds[i];
        const DemoDef* def = &DEMO[i];
        /* the updater runs 25 s of every 55 s, so starts and exits show up */
        bool alive = i != NDEMO - 1 || fmod(t, 55.0) < 25.0;
        if (alive && !d->alive) {
            d->pid = i == 0 ? 4 : (uint32_t)(1000 + i * 412 + ((int)(t / 55.0) % 7) * 4096 * (i == NDEMO - 1));
            d->create = i == NDEMO - 1 ? now64 : now64 - (uint64_t)(36000.0 + i * 37.0) * 10000000ULL;
            d->cpu = d->io_r = d->io_w = 0;
        }
        d->alive = alive;
        if (!alive) continue;
        float n = demo_noise(i, t);
        float cpu = def->cpu + def->wobble * n;
        if (i == 33 && fmod(t, 40.0) < 6.0) cpu += 35.0f;           /* clangd re-index burst */
        if (i == 15 && fmod(t + 20.0, 90.0) < 8.0) cpu += 22.0f;    /* Defender scan */
        if (cpu < 0) cpu = 0;
        d->cpu += (uint64_t)(cpu / 100.0 * ncores * dt * 1e7);
        float io = def->io_kbps * 1024.0f * (0.4f + n);
        d->io_r += (uint64_t)(io * 0.7f * dt);
        d->io_w += (uint64_t)(io * 0.3f * dt);
        float mem = def->mem_mb * (0.96f + 0.08f * n);
        if (i == 37) mem += (float)(t * 0.35);                       /* Slack slowly leaks */
        if (S.nproc == S.proc_cap) {
            S.proc_cap = S.proc_cap ? S.proc_cap * 2 : 512;
            S.procs = realloc(S.procs, sizeof *S.procs * S.proc_cap);
        }
        RawProc* r = &S.procs[S.nproc++];
        memset(r, 0, sizeof *r);
        r->pid = d->pid;
        r->ppid = def->parent >= 0 ? g_ds[def->parent].pid : 0;
        r->threads = (uint32_t)def->threads;
        r->handles = (uint32_t)(def->threads * 31 + i * 7);
        r->session = (!strcmp(def->user, "demo") || !strncmp(def->user, "DWM", 3) || !strncmp(def->user, "UMFD", 4)) ? 1 : 0;
        r->base_prio = i == 16 ? 13 : 8;
        r->create = d->create;
        r->cpu = d->cpu;
        r->ws_private = (uint64_t)(mem * 1048576.0f);
        r->ws = r->ws_private + (uint64_t)(mem * 0.35f * 1048576.0f);
        r->ws_peak = r->ws + r->ws / 8;
        r->commit = r->ws_private + r->ws_private / 4;
        r->virt = r->commit * 6;
        r->paged = 180000 + (uint64_t)i * 4000;
        r->nonpaged = 14000 + (uint64_t)i * 300;
        r->io_r = d->io_r;
        r->io_w = d->io_w;
        r->page_faults = (uint32_t)(t * 40 + i * 1000);
        snprintf(r->name, sizeof r->name, "%s", def->name);
        if (def->gpu > 0) pidgpu_add(d->pid, def->gpu * (0.6f + 0.8f * n), (uint64_t)(def->mem_mb * 0.4f * 1048576.0f));
    }
}

static void demo_apps(void)
{
    S.napp = 0;
    for (int i = 0; i < NDEMO; ++i) {
        if (!DEMO[i].title || !g_ds[i].alive) continue;
        if (S.napp == S.app_cap) {
            S.app_cap = S.app_cap ? S.app_cap * 2 : 64;
            S.apps = realloc(S.apps, sizeof *S.apps * S.app_cap);
        }
        S.apps[S.napp].pid = g_ds[i].pid;
        snprintf(S.apps[S.napp].title, sizeof S.apps[S.napp].title, "%s", DEMO[i].title);
        S.napp++;
    }
}

static void demo_meta(MetaJob* j)
{
    for (int i = 0; i < NDEMO; ++i) {
        if (g_ds[i].pid != j->pid || !g_ds[i].alive) continue;
        const DemoDef* def = &DEMO[i];
        const char* path = def->path ? def->path : g_self_path;
        snprintf(j->path, sizeof j->path, "%s", path);
        snprintf(j->desc, sizeof j->desc, "%s", def->desc);
        snprintf(j->user, sizeof j->user, "%s", def->user);
        snprintf(j->company, sizeof j->company, "%s",
                 strstr(path, "Windows") ? "Microsoft Corporation" : def->path ? "" : "Fezcode");
        snprintf(j->cmdline, sizeof j->cmdline, path[0] ? "\"%s\"" : "", path);
        j->icon = -1;
        if (path[0]) {
            const PathMeta* m = path_meta(path);   /* real file => its real icon */
            if (m) j->icon = m->icon;
        }
        return;
    }
}

#define MAPSZ 8192
static Proc* g_map[MAPSZ];

static void map_build(SysState* st)
{
    memset(g_map, 0, sizeof g_map);
    for (int i = 0; i < st->nproc; ++i) {
        Proc* p = st->procs[i];
        if (!p->alive) continue;
        uint32_t k = (p->pid >> 2) & (MAPSZ - 1);
        while (g_map[k]) k = (k + 1) & (MAPSZ - 1);
        g_map[k] = p;
    }
}

static Proc* map_find(uint32_t pid)
{
    uint32_t k = (pid >> 2) & (MAPSZ - 1);
    while (g_map[k]) {
        if (g_map[k]->pid == pid) return g_map[k];
        k = (k + 1) & (MAPSZ - 1);
    }
    return NULL;
}

static const PidGpu* pidgpu_find(uint32_t pid)
{
    for (int i = 0; i < S.npidgpu; ++i) if (S.pidgpu[i].pid == pid) return &S.pidgpu[i];
    return NULL;
}

static TmCpuPerf g_prev_cores[MAX_CORES];
static bool      g_have_prev_cores;

static void apply_tick(SysState* st, double dt)
{
    const uint64_t k = st->samples;
    const int ring = (int)(k % HIST_LEN);
    st->sample_time[ring] = sys_now_ms();
    st->uptime_ms = GetTickCount64();
    ++g_gen;

    /* --- CPU --- */
    CpuInfo* c = &st->cpu;
    int ncores = S.ncores < MAX_CORES ? S.ncores : MAX_CORES;
    if (ncores > 0) c->logical = ncores;
    double tot_busy = 0, tot_all = 0, tot_kern = 0;
    for (int i = 0; i < ncores; ++i) {
        float u = 0;
        if (g_have_prev_cores) {
            double idle = (double)(S.cores[i].IdleTime.QuadPart - g_prev_cores[i].IdleTime.QuadPart);
            double kern = (double)(S.cores[i].KernelTime.QuadPart - g_prev_cores[i].KernelTime.QuadPart);
            double user = (double)(S.cores[i].UserTime.QuadPart - g_prev_cores[i].UserTime.QuadPart);
            double all = kern + user;
            if (all > 0) {
                u = (float)((all - idle) / all * 100.0);
                tot_busy += all - idle;
                tot_all  += all;
                tot_kern += kern - idle;
            }
        }
        if (u < 0) u = 0;
        if (u > 100) u = 100;
        c->core[i] = u;
        c->h_core[i].v[ring] = u;
        g_prev_cores[i] = S.cores[i];
    }
    g_have_prev_cores = ncores > 0;
    c->usage  = tot_all > 0 ? (float)(tot_busy / tot_all * 100.0) : 0;
    c->kernel = tot_all > 0 ? (float)(tot_kern / tot_all * 100.0) : 0;
    if (c->kernel < 0) c->kernel = 0;
    c->mhz = S.perf > 0 ? (float)(c->base_mhz * S.perf / 100.0) : (float)c->base_mhz;
    c->h_usage.v[ring]  = c->usage;
    c->h_kernel.v[ring] = c->kernel;
    c->temp_ok = S.temps.cpu_ok;
    c->temp = S.temps.cpu;
    snprintf(c->temp_src, sizeof c->temp_src, "%s", S.temps.src);
    c->h_temp.v[ring] = c->temp_ok ? c->temp : 0;

    /* --- processes --- */
    map_build(st);
    const double cpu_scale = 100.0 / (dt * 1e7 * (ncores > 0 ? ncores : 1));
    float total_hf = 0;
    int nthreads = 0, nhandles = 0, nalive = 0;
    uint64_t compressed = 0;
    for (int i = 0; i < S.nproc; ++i) {
        RawProc* r = &S.procs[i];
        if (r->pid == 0) continue;               /* the idle process is just inverted CPU */
        nthreads += (int)r->threads;
        nhandles += (int)r->handles;
        nalive++;
        Proc* p = map_find(r->pid);
        if (p && p->create_time != r->create) p = NULL;
        bool fresh = p == NULL;
        if (fresh) {
            p = calloc(1, sizeof *p);
            if (!p) continue;
            p->pid = r->pid;
            p->create_time = r->create;
            p->icon = -1;
            p->first_sample = k;
            snprintf(p->name, sizeof p->name, "%s", r->name);
            snprintf(p->desc, sizeof p->desc, "%s", r->name);
            p->group = PGROUP_BACKGROUND;
            if (st->nproc == st->proc_cap) {
                st->proc_cap = st->proc_cap ? st->proc_cap * 2 : 512;
                st->procs = realloc(st->procs, sizeof *st->procs * st->proc_cap);
            }
            st->procs[st->nproc++] = p;
            if (k > 0) sys_log(LOG_LV_INFO, "Process", p->pid, false, "Started %s (PID %u, parent %u)", p->name, p->pid, r->ppid);
        }
        if (!fresh && dt > 0) {
            p->cpu = r->cpu >= p->cpu_time ? (float)((r->cpu - p->cpu_time) * cpu_scale) : 0;
            if (p->cpu > 100) p->cpu = 100;
            p->io_read_bps  = r->io_r >= p->io_read  ? (float)((r->io_r - p->io_read) / dt) : 0;
            p->io_write_bps = r->io_w >= p->io_write ? (float)((r->io_w - p->io_write) / dt) : 0;
            p->io_other_bps = r->io_o >= p->io_other ? (float)((r->io_o - p->io_other) / dt) : 0;
            p->hard_faults_ps = r->hard_faults >= p->hard_faults ? (float)((r->hard_faults - p->hard_faults) / dt) : 0;
        }
        total_hf += p->hard_faults_ps;
        p->ppid = r->ppid;
        p->threads = r->threads;
        p->handles = r->handles;
        p->session = r->session;
        p->base_prio = r->base_prio;
        p->ws_private = r->ws_private;
        p->ws = r->ws;
        p->ws_peak = r->ws_peak;
        p->commit = r->commit;
        p->virt = r->virt;
        p->paged_pool = r->paged;
        p->nonpaged_pool = r->nonpaged;
        p->page_faults = r->page_faults;
        p->hard_faults = r->hard_faults;
        p->cpu_time = r->cpu;
        p->io_read = r->io_r;
        p->io_write = r->io_w;
        p->io_other = r->io_o;
        p->is_suspended = r->suspended;
        const PidGpu* g = pidgpu_find(r->pid);
        p->gpu = g ? (g->util > 100 ? 100 : g->util) : 0;
        p->gpu_mem = g ? g->mem : 0;
        p->alive = true;
        p->seen = g_gen;
        p->last_sample = k;
        p->h_cpu[ring] = p->cpu;
        p->h_mem[ring] = (float)p->ws_private;
        p->h_io[ring]  = p->io_read_bps + p->io_write_bps;
        p->h_gpu[ring] = p->gpu;
        if (!strcmp(p->name, "Memory Compression")) compressed = p->ws;

        p->is_app = false;
        for (int a = 0; a < S.napp; ++a)
            if (S.apps[a].pid == p->pid) {
                p->is_app = true;
                snprintf(p->title, sizeof p->title, "%s", S.apps[a].title);
                break;
            }
        if (p->is_app) p->group = PGROUP_APP;
        else if (p->meta_done && (!p->path[0] || starts_with_ci(p->path, g_windir))) p->group = PGROUP_WINDOWS;
        else if (p->pid <= 4 || !strcmp(p->name, "Registry") || !strcmp(p->name, "Secure System") ||
                 !strcmp(p->name, "Memory Compression")) p->group = PGROUP_WINDOWS;
        else p->group = PGROUP_BACKGROUND;
    }
    if (g_demo) {
        float sum = 0;
        for (int i = 0; i < st->nproc; ++i) if (st->procs[i]->alive) sum += st->procs[i]->cpu;
        float u = sum + 1.5f > 100 ? 100 : sum + 1.5f;
        c->usage = u;
        c->kernel = u * 0.22f;
        for (int i = 0; i < ncores; ++i) {
            float v = u * (0.55f + 0.9f * demo_noise(i + 50, g_demo_t));
            c->core[i] = v > 100 ? 100 : v;
            c->h_core[i].v[ring] = c->core[i];
        }
        c->h_usage.v[ring] = c->usage;
        c->h_kernel.v[ring] = c->kernel;
    }
    st->total_procs = nalive;
    st->total_threads = nthreads;
    st->total_handles = nhandles;

    /* exits and expiry */
    int w = 0;
    for (int i = 0; i < st->nproc; ++i) {
        Proc* p = st->procs[i];
        if (p->alive && p->seen != g_gen) {
            p->alive = false;
            float cpu_s = (float)(p->cpu_time / 1e7);
            sys_log(LOG_LV_INFO, "Process", p->pid, false, "Exited %s (PID %u) after %.1f s CPU", p->name, p->pid, cpu_s);
        }
        if (!p->alive && k - p->last_sample >= HIST_LEN) { free(p); continue; }
        st->procs[w++] = p;
    }
    st->nproc = w;

    /* --- memory --- */
    MemInfo* m = &st->mem;
    m->total = S.ms.ullTotalPhys;
    m->avail = S.ms.ullAvailPhys;
    m->used  = m->total - m->avail;
    m->load  = m->total ? (float)((double)m->used / m->total * 100.0) : 0;
    m->hw_reserved = m->installed > m->total ? m->installed - m->total : 0;
    uint64_t page = S.pi.PageSize ? S.pi.PageSize : 4096;
    m->commit       = (uint64_t)S.pi.CommitTotal * page;
    m->commit_limit = (uint64_t)S.pi.CommitLimit * page;
    m->paged_pool   = (uint64_t)S.pi.KernelPaged * page;
    m->nonpaged_pool= (uint64_t)S.pi.KernelNonpaged * page;
    m->standby  = S.standby;
    m->modified = S.modified;
    m->free     = S.freezero;
    m->cached   = m->standby + m->modified;
    m->compressed = compressed;
    m->hard_faults_ps = total_hf;
    m->h_used.v[ring]   = (float)m->used;
    m->h_commit.v[ring] = (float)m->commit;
    m->h_hard_faults.v[ring] = total_hf;

    /* --- disks --- */
    for (int i = 0; i < S.ndisk; ++i) {
        RawDisk* rd = &S.disks[i];
        DiskInfo* d = NULL;
        for (int j = 0; j < st->ndisk; ++j) if (st->disk[j].index == rd->index) d = &st->disk[j];
        if (!d) {
            if (st->ndisk >= MAX_DISKS) continue;
            d = &st->disk[st->ndisk++];
            memset(d, 0, sizeof *d);
            d->index = rd->index;
            snprintf(d->name, sizeof d->name, "Disk %d", rd->index);
            disk_static(d);
            /* keep disks ordered by index */
            for (int j = st->ndisk - 1; j > 0 && st->disk[j - 1].index > st->disk[j].index; --j) {
                DiskInfo t = st->disk[j]; st->disk[j] = st->disk[j - 1]; st->disk[j - 1] = t;
            }
            for (int j = 0; j < st->ndisk; ++j) if (st->disk[j].index == rd->index) d = &st->disk[j];
        }
        snprintf(d->letters, sizeof d->letters, "%s", rd->letters);
        d->system = g_windir[0] && strchr(d->letters, g_windir[0] - 32) != NULL;
        d->active = rd->active;
        d->read_bps = rd->read;
        d->write_bps = rd->write;
        d->resp_ms = rd->resp_ms;
        d->queue = rd->queue;
        d->h_active.v[ring] = d->active;
        d->h_read.v[ring]   = d->read_bps;
        d->h_write.v[ring]  = d->write_bps;
    }

    /* --- network --- */
    if (S.ift) {
        NetInfo fresh_list[MAX_NETS];
        int nf = 0;
        for (ULONG i = 0; i < S.ift->NumEntries && nf < MAX_NETS; ++i) {
            MIB_IF_ROW2* r = &S.ift->Table[i];
            if (!r->InterfaceAndOperStatusFlags.HardwareInterface) continue;
            if (r->InterfaceAndOperStatusFlags.FilterInterface) continue;
            if (r->OperStatus != IfOperStatusUp) continue;
            if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            NetInfo* old = NULL;
            for (int j = 0; j < st->nnet; ++j) if (st->net[j].luid == r->InterfaceLuid.Value) old = &st->net[j];
            NetInfo* n = &fresh_list[nf++];
            if (old) *n = *old;
            else {
                memset(n, 0, sizeof *n);
                n->luid = r->InterfaceLuid.Value;
            }
            w2u(r->Alias, n->name, sizeof n->name);
            w2u(r->Description, n->desc, sizeof n->desc);
            snprintf(n->kind, sizeof n->kind, "%s",
                     r->Type == IF_TYPE_IEEE80211 ? "Wi-Fi" : r->Type == IF_TYPE_ETHERNET_CSMACD ? "Ethernet"
                     : r->Type == IF_TYPE_WWANPP || r->Type == IF_TYPE_WWANPP2 ? "Cellular" : "Network");
            n->link_bps = r->ReceiveLinkSpeed;
            if (r->PhysicalAddressLength == 6)
                snprintf(n->mac, sizeof n->mac, "%02X-%02X-%02X-%02X-%02X-%02X",
                         r->PhysicalAddress[0], r->PhysicalAddress[1], r->PhysicalAddress[2],
                         r->PhysicalAddress[3], r->PhysicalAddress[4], r->PhysicalAddress[5]);
            if (old && dt > 0) {
                n->recv_bps = r->InOctets >= old->in_octets ? (float)((r->InOctets - old->in_octets) / dt) : 0;
                n->send_bps = r->OutOctets >= old->out_octets ? (float)((r->OutOctets - old->out_octets) / dt) : 0;
            } else {
                n->recv_bps = n->send_bps = 0;
            }
            n->in_octets = r->InOctets;
            n->out_octets = r->OutOctets;
            n->h_recv.v[ring] = n->recv_bps;
            n->h_send.v[ring] = n->send_bps;
        }
        bool changed = nf != st->nnet;
        for (int i = 0; !changed && i < nf; ++i) if (fresh_list[i].luid != st->net[i].luid) changed = true;
        memcpy(st->net, fresh_list, sizeof fresh_list[0] * nf);
        st->nnet = nf;
        if (changed || k % 15 == 0) fill_adapter_addresses(st);
    }

    /* --- GPU --- */
    for (int i = 0; i < st->ngpu; ++i) {
        GpuInfo* g = &st->gpu[i];
        g->util = S.gpu_util[i];
        for (int e = 0; e < GPU_ENGINES; ++e) {
            g->eng[e] = S.gpu_eng[i][e];
            g->h_eng[e].v[ring] = g->eng[e];
        }
        g->ded_used = S.gpu_ded[i];
        g->shared_used = S.gpu_shared[i];
        g->h_util.v[ring] = g->util;
        g->h_ded.v[ring] = (float)g->ded_used;
        g->h_shared.v[ring] = (float)g->shared_used;
        g->temp_ok = S.temps.gpu_ok[i];
        g->temp = S.temps.gpu[i];
        g->fan_rpm = S.temps.fan[i];
        g->power_pct = S.temps.power[i];
        g->mem_freq = S.temps.memf[i];
        g->h_temp.v[ring] = g->temp_ok ? g->temp : 0;
    }

    st->samples = k + 1;
}

static DWORD WINAPI sampler_main(void* arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    init_cpu_static(&g_st.cpu);
    init_mem_static(&g_st.mem);
    init_gpu_static(&g_st);
    pdh_init();

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    if (g_demo) demo_processes(0); else gather_processes();
    gather_cores();
    sys_lock();
    apply_tick(&g_st, 0);
    sys_unlock();
    /* The first tick has no deltas; throw its rates away by restarting the
     * history at the second one, so graphs never begin with a fake zero. */
    sys_lock();
    g_st.samples = 0;
    for (int i = 0; i < g_st.nproc; ++i) g_st.procs[i]->first_sample = 0;
    sys_unlock();

    ULONGLONG next = GetTickCount64() + 1000;
    uint64_t tick = 0;
    MetaJob* jobs = NULL;
    int jobs_cap = 0;
    VolumeInfo vols[MAX_VOLUMES];
    SessionInfo sess[MAX_SESSIONS];

    while (!g_quit) {
        ULONGLONG t = GetTickCount64();
        if (t < next) { Sleep((DWORD)(next - t > 50 ? 50 : next - t)); continue; }
        next += 1000;
        if (t > next + 2000) next = t + 1000;

        sys_lock();
        bool paused = g_st.paused;
        sys_unlock();
        if (paused) { QueryPerformanceCounter(&last); continue; }

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;

        gather_cores();
        gather_pdh(&g_st);
        if (g_demo) { S.npidgpu = 0; demo_processes(dt); demo_apps(); }
        else { gather_processes(); gather_apps(); }
        gather_net();
        gather_temps(&g_st, &S.temps, tick);
        S.ms.dwLength = sizeof S.ms;
        GlobalMemoryStatusEx(&S.ms);
        S.pi.cb = sizeof S.pi;
        GetPerformanceInfo(&S.pi, sizeof S.pi);

        int want = (int)g_want;
        int nvol = -1, nsess = -1, nconn = 0, nsvc = 0, nstart = 0;
        ConnInfo* conns = NULL;
        ServiceInfo* svcs = NULL;
        StartupItem* startup = NULL;
        bool got_conns = false, got_svcs = false, got_startup = false;
        if (tick % 5 == 0) gather_volumes(vols, &nvol);
        if (tick % 3 == 0 || (want & SYSWANT_SESSIONS)) gather_sessions(sess, &nsess);
        if (g_demo && nsess > 0) {
            nsess = 1;
            sess[0].id = 1;
            snprintf(sess[0].user, sizeof sess[0].user, "demo");
            snprintf(sess[0].station, sizeof sess[0].station, "Console");
            snprintf(sess[0].state, sizeof sess[0].state, "Active");
        }
        if ((want & SYSWANT_CONNS) && tick % 2 == 0) { conns = gather_conns(&nconn); got_conns = true; }
        static uint64_t svc_polls;
        if ((want & SYSWANT_SERVICES) && (tick % 2 == 0 || !g_st.nsvc)) {
            svcs = gather_services(&nsvc, svc_polls++ % 15 == 0);
            got_svcs = true;
        }
        if ((want & SYSWANT_STARTUP) && InterlockedExchange(&g_startup_dirty, 0)) {
            startup = gather_startup(&nstart);
            got_startup = true;
        }

        sys_lock();
        apply_tick(&g_st, dt);
        if (nvol >= 0)  { memcpy(g_st.vol, vols, sizeof vols[0] * nvol); g_st.nvol = nvol; }
        if (nsess >= 0) { memcpy(g_st.sessions, sess, sizeof sess[0] * nsess); g_st.nsession = nsess; }
        if (got_conns)  { free(g_st.conns); g_st.conns = conns; g_st.nconn = nconn; }
        if (got_svcs && svcs) { free(g_st.svcs); g_st.svcs = svcs; g_st.nsvc = nsvc; }
        if (got_startup){ free(g_st.startup); g_st.startup = startup; g_st.nstartup = nstart; }

        /* collect processes still missing metadata */
        int njobs = 0;
        for (int i = 0; i < g_st.nproc; ++i) {
            Proc* p = g_st.procs[i];
            if (p->meta_done || !p->alive) continue;
            if (njobs == jobs_cap) {
                jobs_cap = jobs_cap ? jobs_cap * 2 : 256;
                jobs = realloc(jobs, sizeof *jobs * jobs_cap);
            }
            memset(&jobs[njobs], 0, sizeof jobs[0]);
            jobs[njobs].pid = p->pid;
            jobs[njobs].create = p->create_time;
            njobs++;
        }
        sys_unlock();

        /* resolve them off-lock, with a time budget so one slow tick of icon
         * extraction never stalls the clock */
        ULONGLONG budget = GetTickCount64() + 350;
        int done = 0;
        for (; done < njobs && GetTickCount64() < budget; ++done) {
            if (g_demo) demo_meta(&jobs[done]); else resolve_meta(&jobs[done]);
        }
        if (done) {
            sys_lock();
            map_build(&g_st);
            for (int j = 0; j < done; ++j) {
                Proc* p = map_find(jobs[j].pid);
                if (!p || p->create_time != jobs[j].create) continue;
                snprintf(p->path, sizeof p->path, "%s", jobs[j].path);
                if (jobs[j].desc[0]) snprintf(p->desc, sizeof p->desc, "%s", jobs[j].desc);
                snprintf(p->company, sizeof p->company, "%s", jobs[j].company);
                snprintf(p->user, sizeof p->user, "%s", jobs[j].user);
                snprintf(p->cmdline, sizeof p->cmdline, "%s", jobs[j].cmdline);
                p->icon = jobs[j].icon;
                p->meta_done = true;
            }
            sys_unlock();
        }
        ++tick;
    }
    free(jobs);
    CoUninitialize();
    return 0;
}

SysState* sys_start(void)
{
    InitializeCriticalSection(&g_cs);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    pNtQSI     = (PFN_NtQSI)(void*)GetProcAddress(nt, "NtQuerySystemInformation");
    pNtQIP     = (PFN_NtQIP)(void*)GetProcAddress(nt, "NtQueryInformationProcess");
    pNtSuspend = (PFN_NtProc)(void*)GetProcAddress(nt, "NtSuspendProcess");
    pNtResume  = (PFN_NtProc)(void*)GetProcAddress(nt, "NtResumeProcess");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    pSetProcInfo = (PFN_ProcInfo)(void*)GetProcAddress(k32, "SetProcessInformation");
    pGetProcInfo = (PFN_ProcInfo)(void*)GetProcAddress(k32, "GetProcessInformation");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    pKmtOpen  = (PFN_KmtOpen)(void*)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
    pKmtQuery = (PFN_KmtQuery)(void*)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    enable_debug_privilege();
    g_st.elevated = is_elevated();
    g_st.self_pid = GetCurrentProcessId();
    init_os_static(&g_st);
    if (g_demo) {
        snprintf(g_st.computer, sizeof g_st.computer, "DEMO-PC");
        snprintf(g_st.user, sizeof g_st.user, "demo");
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        w2u(self, g_self_path, sizeof g_self_path);
    }

    sys_log(LOG_LV_INFO, "Tarman", g_st.self_pid, true, "Started%s on %s", g_st.elevated ? " elevated" : "", g_st.os_name);
    g_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    return &g_st;
}

void sys_stop(void)
{
    InterlockedExchange(&g_quit, 1);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

/* ---- actions ------------------------------------------------------------------- */

static HANDLE open_proc(uint32_t pid, DWORD access, char* err, int errn)
{
    if (g_demo) { set_msg(err, errn, "Demo mode: process actions are disabled."); return NULL; }
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) set_err(err, errn, GetLastError());
    return h;
}

bool sys_kill(uint32_t pid, char* err, int errn)
{
    HANDLE h = open_proc(pid, PROCESS_TERMINATE, err, errn);
    if (!h) return false;
    bool ok = TerminateProcess(h, 1) != 0;
    if (!ok) set_err(err, errn, GetLastError());
    CloseHandle(h);
    return ok;
}

static void kill_children(uint32_t pid, PROCESSENTRY32W* all, int n, int depth)
{
    if (depth > 64) return;
    for (int i = 0; i < n; ++i)
        if (all[i].th32ParentProcessID == pid && all[i].th32ProcessID != pid && all[i].th32ProcessID) {
            kill_children(all[i].th32ProcessID, all, n, depth + 1);
            sys_kill(all[i].th32ProcessID, NULL, 0);
        }
}

bool sys_kill_tree(uint32_t pid, char* err, int errn)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        int cap = 512, n = 0;
        PROCESSENTRY32W* all = malloc(sizeof *all * cap);
        PROCESSENTRY32W e = { sizeof e };
        if (all && Process32FirstW(snap, &e)) {
            do {
                if (n == cap) { cap *= 2; all = realloc(all, sizeof *all * cap); }
                all[n++] = e;
            } while (Process32NextW(snap, &e));
        }
        if (all) kill_children(pid, all, n, 0);
        free(all);
        CloseHandle(snap);
    }
    return sys_kill(pid, err, errn);
}

bool sys_suspend(uint32_t pid, bool resume, char* err, int errn)
{
    if (!pNtSuspend || !pNtResume) { set_msg(err, errn, "Not supported."); return false; }
    HANDLE h = open_proc(pid, PROCESS_SUSPEND_RESUME, err, errn);
    if (!h) return false;
    LONG r = resume ? pNtResume(h) : pNtSuspend(h);
    CloseHandle(h);
    if (r < 0) { set_msg(err, errn, "The process refused to change state."); return false; }
    return true;
}

bool sys_set_priority(uint32_t pid, uint32_t cls, char* err, int errn)
{
    HANDLE h = open_proc(pid, PROCESS_SET_INFORMATION, err, errn);
    if (!h) return false;
    bool ok = SetPriorityClass(h, cls) != 0;
    if (!ok) set_err(err, errn, GetLastError());
    CloseHandle(h);
    return ok;
}

bool sys_set_affinity(uint32_t pid, uint64_t mask, char* err, int errn)
{
    if (!mask) { set_msg(err, errn, "At least one processor must be selected."); return false; }
    HANDLE h = open_proc(pid, PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, err, errn);
    if (!h) return false;
    bool ok = SetProcessAffinityMask(h, (DWORD_PTR)mask) != 0;
    if (!ok) set_err(err, errn, GetLastError());
    CloseHandle(h);
    return ok;
}

bool sys_set_efficiency(uint32_t pid, bool on, char* err, int errn)
{
    if (!pSetProcInfo) { set_msg(err, errn, "Efficiency mode needs Windows 11."); return false; }
    HANDLE h = open_proc(pid, PROCESS_SET_INFORMATION, err, errn);
    if (!h) return false;
    TmThrottle t = { 1, 1, on ? 1u : 0u };
    bool ok = pSetProcInfo(h, TM_ProcessPowerThrottling, &t, sizeof t) != 0;
    if (ok) SetPriorityClass(h, on ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    else set_err(err, errn, GetLastError());
    CloseHandle(h);
    return ok;
}

bool sys_create_dump(uint32_t pid, const char* name, char* out_path, int outn, char* err, int errn)
{
    HANDLE h = open_proc(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, err, errn);
    if (!h) return false;
    wchar_t tmp[MAX_PATH], path[MAX_PATH], wname[64];
    GetTempPathW(MAX_PATH, tmp);
    u2w(name, wname, 64);
    wchar_t* dot = wcsrchr(wname, L'.');
    if (dot) *dot = 0;
    _snwprintf(path, MAX_PATH, L"%ls%ls.DMP", tmp, wname);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { set_err(err, errn, GetLastError()); CloseHandle(h); return false; }
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                         MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo);
    bool ok = MiniDumpWriteDump(h, pid, f, type, NULL, NULL, NULL) != 0;
    if (!ok) set_err(err, errn, GetLastError());
    CloseHandle(f);
    CloseHandle(h);
    if (!ok) DeleteFileW(path);
    else w2u(path, out_path, outn);
    return ok;
}

bool sys_run(const char* cmd, bool admin, char* err, int errn)
{
    wchar_t w[1024];
    u2w(cmd, w, 1024);
    wchar_t* p = w;
    while (*p == L' ') ++p;
    wchar_t* params = NULL;
    if (*p == L'"') {
        ++p;
        wchar_t* q = wcschr(p, L'"');
        if (q) { *q = 0; params = q + 1; }
    } else {
        wchar_t* q = wcschr(p, L' ');
        if (q) { *q = 0; params = q + 1; }
    }
    if (params) while (*params == L' ') ++params;
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    se.lpVerb = admin ? L"runas" : L"open";
    se.lpFile = p;
    se.lpParameters = params && *params ? params : NULL;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) { set_err(err, errn, GetLastError()); return false; }
    return true;
}

static DWORD WINAPI service_restart_thread(void* arg)
{
    wchar_t* name = arg;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE s = scm ? OpenServiceW(scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS) : NULL;
    if (s) {
        SERVICE_STATUS ss;
        ControlService(s, SERVICE_CONTROL_STOP, &ss);
        for (int i = 0; i < 100; ++i) {
            if (QueryServiceStatus(s, &ss) && ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
        StartServiceW(s, 0, NULL);
        CloseServiceHandle(s);
    }
    if (scm) CloseServiceHandle(scm);
    free(name);
    return 0;
}

bool sys_service_control(const char* name, int op, char* err, int errn)
{
    wchar_t w[128];
    u2w(name, w, 128);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { set_err(err, errn, GetLastError()); return false; }
    SC_HANDLE s = OpenServiceW(scm, w, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s) { set_err(err, errn, GetLastError()); CloseServiceHandle(scm); return false; }
    bool ok = true;
    if (op == 0) {
        ok = StartServiceW(s, 0, NULL) != 0;
    } else if (op == 1) {
        SERVICE_STATUS ss;
        ok = ControlService(s, SERVICE_CONTROL_STOP, &ss) != 0;
    } else {
        wchar_t* copy = _wcsdup(w);
        HANDLE t = copy ? CreateThread(NULL, 0, service_restart_thread, copy, 0, NULL) : NULL;
        if (t) CloseHandle(t); else free(copy);
    }
    if (!ok) set_err(err, errn, GetLastError());
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

bool sys_startup_set(const StartupItem* it, bool enabled, char* err, int errn)
{
    HKEY h;
    REGSAM sam = KEY_SET_VALUE | KEY_WOW64_64KEY;
    LONG r = RegCreateKeyExW(source_root(it->source), APPROVED_KEY[it->source], 0, NULL, 0, sam, NULL, &h, NULL);
    if (r != ERROR_SUCCESS) { set_err(err, errn, (DWORD)r); return false; }
    uint8_t data[12] = { 0 };
    data[0] = enabled ? 0x02 : 0x03;
    if (!enabled) {
        FILETIME f;
        GetSystemTimeAsFileTime(&f);
        memcpy(data + 4, &f, 8);
    }
    wchar_t name[128];
    u2w(it->name, name, 128);
    r = RegSetValueExW(h, name, 0, REG_BINARY, data, sizeof data);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS) { set_err(err, errn, (DWORD)r); return false; }
    sys_refresh_startup();
    return true;
}

bool sys_session_action(uint32_t id, int op, char* err, int errn)
{
    BOOL ok = op == 0 ? WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE)
                      : WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE);
    if (!ok) set_err(err, errn, GetLastError());
    return ok != 0;
}

void sys_open_location(const char* path)
{
    wchar_t w[300], args[340];
    u2w(path, w, 300);
    _snwprintf(args, 340, L"/select,\"%ls\"", w);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

void sys_open_properties(const char* path)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t w[300];
    u2w(path, w, 300);
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_INVOKEIDLIST;
    se.lpVerb = L"properties";
    se.lpFile = w;
    se.nShow = SW_SHOW;
    ShellExecuteExW(&se);
}

void sys_search_online(const char* name)
{
    char url[512] = "https://www.bing.com/search?q=";
    size_t o = strlen(url);
    for (const char* p = name; *p && o + 4 < sizeof url; ++p) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')
            url[o++] = (char)ch;
        else o += (size_t)snprintf(url + o, sizeof url - o, "%%%02X", ch);
    }
    url[o] = 0;
    wchar_t w[512];
    u2w(url, w, 512);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

void sys_open_tool(const char* tool)
{
    wchar_t w[128];
    u2w(tool, w, 128);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

bool sys_restart_elevated(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    SHELLEXECUTEINFOW se = { sizeof se };
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&se) != 0;
}

void sys_proc_extra(uint32_t pid, ProcExtra* out)
{
    memset(out, 0, sizeof *out);
    if (g_demo) {
        out->ok = true;
        out->prio_class = NORMAL_PRIORITY_CLASS;
        out->gdi = 40 + pid % 300;
        out->user_objs = 20 + pid % 90;
        out->affinity = out->sys_affinity = (1ULL << (g_st.cpu.logical < 64 ? g_st.cpu.logical : 63)) - 1;
        return;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    out->ok = true;
    out->prio_class = GetPriorityClass(h);
    out->gdi = GetGuiResources(h, GR_GDIOBJECTS);
    out->user_objs = GetGuiResources(h, GR_USEROBJECTS);
    DWORD_PTR pm = 0, sm = 0;
    if (GetProcessAffinityMask(h, &pm, &sm)) { out->affinity = pm; out->sys_affinity = sm; }
    BOOL wow = FALSE;
    IsWow64Process(h, &wow);
    out->wow64 = wow != 0;
    if (pGetProcInfo) {
        TmThrottle t = { 1, 0, 0 };
        if (pGetProcInfo(h, TM_ProcessPowerThrottling, &t, sizeof t))
            out->efficiency = (t.ControlMask & 1) && (t.StateMask & 1);
    }
    CloseHandle(h);
}

int sys_modules(uint32_t pid, ModuleInfo* out, int max)
{
    if (g_demo) return -1;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    MODULEENTRY32W e = { sizeof e };
    int n = 0;
    if (Module32FirstW(snap, &e)) {
        do {
            if (n >= max) break;
            w2u(e.szModule, out[n].name, sizeof out[n].name);
            w2u(e.szExePath, out[n].path, sizeof out[n].path);
            out[n].base = (uint64_t)(uintptr_t)e.modBaseAddr;
            out[n].size = e.modBaseSize;
            n++;
        } while (Module32NextW(snap, &e));
    }
    CloseHandle(snap);
    return n;
}

void sys_window_setup(void* hwnd, bool dark, bool topmost)
{
    HWND h = (HWND)hwnd;
    if (!h) return;
    BOOL d = dark;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof d);
    COLORREF cap = dark ? RGB(0x0d, 0x10, 0x15) : RGB(0xf4, 0xf6, 0xf9);
    DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    COLORREF txt = dark ? RGB(0xe6, 0xe9, 0xef) : RGB(0x1a, 0x1f, 0x29);
    DwmSetWindowAttribute(h, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof txt);
    SetWindowPos(h, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
