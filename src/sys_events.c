/* Windows event logs for the Events page, through the Vista+ wevtapi.
 *
 * A query renders the newest matching events (system fields plus the
 * provider's formatted message) on a background thread and appends them to
 * a shared result the page reads under sys_events_lock(). A newer query bumps
 * a generation counter; an older thread notices on its next batch and stops
 * without touching the result. The "Applications and Services" channel list is
 * built the same way, once, since probing ~1000 channels takes a moment. */

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <winevt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sys.h"

static CRITICAL_SECTION g_ecs;
static CRITICAL_SECTION g_pubcs;    /* publisher cache: one formatting thread at a time */
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static EventsState      g_ev;
static volatile LONG    g_gen;

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID* c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_ecs);
    InitializeCriticalSection(&g_pubcs);
    return TRUE;
}

static void ensure_init(void) { InitOnceExecuteOnce(&g_once, init_cs, NULL, NULL); }

EventsState* sys_events_lock(void)
{
    ensure_init();
    EnterCriticalSection(&g_ecs);
    return &g_ev;
}

void sys_events_unlock(void) { LeaveCriticalSection(&g_ecs); }

static void w2u(const wchar_t* w, char* out, int n)
{
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    out[n - 1] = 0;
}

static char* w2u_dup(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* s = malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static int64_t ft_to_ms(ULONGLONG ft) { return (int64_t)((ft - 116444736000000000ULL) / 10000ULL); }

static void set_error(const char* what, DWORD code)
{
    wchar_t buf[200];
    char msg[200];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, buf, 200, NULL);
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) buf[--n] = 0;
    w2u(buf, msg, sizeof msg);
    EnterCriticalSection(&g_ecs);
    snprintf(g_ev.error, sizeof g_ev.error, "%s: %s", what, n ? msg : "unknown error");
    LeaveCriticalSection(&g_ecs);
}

/* ---- publisher metadata cache (message formatting) ------------------------------ */

typedef struct { wchar_t name[128]; EVT_HANDLE h; bool tried; } PubMeta;
static PubMeta g_pubs[512];
static int     g_npubs;

static EVT_HANDLE publisher(const wchar_t* name)
{
    for (int i = 0; i < g_npubs; ++i)
        if (!wcscmp(g_pubs[i].name, name)) return g_pubs[i].h;
    if (g_npubs == 512) return NULL;
    PubMeta* p = &g_pubs[g_npubs++];
    wcsncpy(p->name, name, 127);
    p->name[127] = 0;
    p->h = EvtOpenPublisherMetadata(NULL, name, NULL, 0, 0);
    p->tried = true;
    return p->h;
}

static wchar_t* format_message(EVT_HANDLE pub, EVT_HANDLE ev, EVT_FORMAT_MESSAGE_FLAGS what)
{
    if (!pub) return NULL;
    DWORD used = 0;
    EvtFormatMessage(pub, ev, 0, 0, NULL, what, 0, NULL, &used);
    if (!used) return NULL;
    wchar_t* buf = malloc(sizeof(wchar_t) * (used + 1));
    if (!buf) return NULL;
    if (!EvtFormatMessage(pub, ev, 0, 0, NULL, what, used, buf, &used)) {
        DWORD e = GetLastError();
        /* a message with unresolved inserts still comes back usable */
        if (e != ERROR_EVT_UNRESOLVED_VALUE_INSERT && e != ERROR_EVT_UNRESOLVED_PARAMETER_INSERT &&
            e != ERROR_EVT_MAX_INSERTS_REACHED) {
            free(buf);
            return NULL;
        }
    }
    buf[used] = 0;
    return buf;
}

/* Fallback when a provider has no message resources: the EventData values. */
static char* event_data_text(EVT_HANDLE ev)
{
    DWORD used = 0, props = 0;
    EvtRender(NULL, ev, EvtRenderEventXml, 0, NULL, &used, &props);
    if (!used) return NULL;
    wchar_t* xml = malloc(used + sizeof(wchar_t));
    if (!xml) return NULL;
    if (!EvtRender(NULL, ev, EvtRenderEventXml, used, xml, &used, &props)) { free(xml); return NULL; }
    /* collect the text of <Data ...>value</Data> elements */
    wchar_t out[1024];
    size_t o = 0;
    for (wchar_t* p = wcsstr(xml, L"<Data"); p && o < 1000; p = wcsstr(p + 5, L"<Data")) {
        wchar_t* gt = wcschr(p, L'>');
        if (!gt || gt[-1] == L'/') continue;
        wchar_t* end = wcsstr(gt, L"</Data>");
        if (!end) break;
        size_t len = (size_t)(end - gt - 1);
        if (!len) continue;
        if (o) { out[o++] = L';'; out[o++] = L' '; }
        if (len > 1000 - o) len = 1000 - o;
        wmemcpy(out + o, gt + 1, len);
        o += len;
    }
    out[o] = 0;
    free(xml);
    if (!o) return NULL;
    return w2u_dup(out);
}

/* ---- queries -------------------------------------------------------------------- */

typedef struct {
    wchar_t  channel[160];
    unsigned mask;
    int      hours, max;
    LONG     gen;
} QueryJob;

static void build_xpath(const QueryJob* j, wchar_t* out, size_t n)
{
    wchar_t lv[160] = L"";
    int count = 0;
    for (int i = 0; i <= 5; ++i) {
        if (!(j->mask & (1u << i))) continue;
        size_t l = wcslen(lv);
        _snwprintf(lv + l, 160 - l, L"%lsLevel=%d", count ? L" or " : L"", i);
        count++;
    }
    bool all_levels = count == 0 || (j->mask & 0x3F) == 0x3F;
    if (all_levels && j->hours <= 0) { _snwprintf(out, n, L"*"); return; }
    wchar_t cond[300] = L"";
    if (!all_levels) _snwprintf(cond, 300, L"(%ls)", lv);
    if (j->hours > 0) {
        size_t l = wcslen(cond);
        _snwprintf(cond + l, 300 - l, L"%lsTimeCreated[timediff(@SystemTime) <= %llu]", l ? L" and " : L"",
                   (unsigned long long)j->hours * 3600000ULL);
    }
    _snwprintf(out, n, L"*[System[%ls]]", cond);
}

static bool still_current(LONG gen) { return gen == g_gen; }

static DWORD WINAPI query_thread(void* arg)
{
    QueryJob* j = arg;
    wchar_t xpath[512];
    build_xpath(j, xpath, 512);
    EVT_HANDLE q = EvtQuery(NULL, j->channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) {
        DWORD e = GetLastError();
        if (still_current(j->gen)) {
            set_error(e == ERROR_ACCESS_DENIED ? "Access denied (this log needs administrator rights)" : "Couldn't open the log", e);
            EnterCriticalSection(&g_ecs);
            g_ev.busy = false;
            LeaveCriticalSection(&g_ecs);
        }
        free(j);
        return 0;
    }
    EVT_HANDLE ctx = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    uint8_t vbuf[8192];
    EVT_HANDLE evs[32];
    DWORD got = 0;
    int total = 0;
    bool truncated = false;
    while (still_current(j->gen) && EvtNext(q, 32, evs, 3000, 0, &got)) {
        EventRec batch[32];
        int nb = 0;
        EnterCriticalSection(&g_pubcs);
        for (DWORD i = 0; i < got; ++i) {
            EventRec* r = &batch[nb];
            memset(r, 0, sizeof *r);
            DWORD used = 0, count = 0;
            if (EvtRender(ctx, evs[i], EvtRenderEventValues, sizeof vbuf, vbuf, &used, &count)) {
                PEVT_VARIANT v = (PEVT_VARIANT)vbuf;
                if (v[EvtSystemProviderName].Type == EvtVarTypeString)
                    w2u(v[EvtSystemProviderName].StringVal, r->provider, sizeof r->provider);
                if (v[EvtSystemEventID].Type == EvtVarTypeUInt16) r->event_id = v[EvtSystemEventID].UInt16Val;
                if (v[EvtSystemLevel].Type == EvtVarTypeByte) r->level = v[EvtSystemLevel].ByteVal;
                if (v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) r->time_ms = ft_to_ms(v[EvtSystemTimeCreated].FileTimeVal);
                if (v[EvtSystemEventRecordId].Type == EvtVarTypeUInt64) r->record_id = v[EvtSystemEventRecordId].UInt64Val;
                if (v[EvtSystemProcessID].Type == EvtVarTypeUInt32) r->pid = v[EvtSystemProcessID].UInt32Val;
                if (v[EvtSystemThreadID].Type == EvtVarTypeUInt32) r->tid = v[EvtSystemThreadID].UInt32Val;
                if (v[EvtSystemComputer].Type == EvtVarTypeString)
                    w2u(v[EvtSystemComputer].StringVal, r->computer, sizeof r->computer);
                EVT_HANDLE pub = v[EvtSystemProviderName].Type == EvtVarTypeString
                                     ? publisher(v[EvtSystemProviderName].StringVal) : NULL;
                wchar_t* msg = format_message(pub, evs[i], EvtFormatMessageEvent);
                if (msg) {
                    /* trim trailing whitespace Windows likes to append */
                    size_t l = wcslen(msg);
                    while (l && (msg[l - 1] == L'\r' || msg[l - 1] == L'\n' || msg[l - 1] == L' ')) msg[--l] = 0;
                    r->message = w2u_dup(msg);
                    free(msg);
                } else {
                    r->message = event_data_text(evs[i]);
                }
                wchar_t* task = format_message(pub, evs[i], EvtFormatMessageTask);
                if (task) { w2u(task, r->task, sizeof r->task); free(task); }
                nb++;
            }
            EvtClose(evs[i]);
        }
        LeaveCriticalSection(&g_pubcs);
        EnterCriticalSection(&g_ecs);
        if (still_current(j->gen)) {
            EventRec* grown = realloc(g_ev.recs, sizeof *g_ev.recs * (size_t)(g_ev.n + nb));
            if (grown) {
                g_ev.recs = grown;
                memcpy(g_ev.recs + g_ev.n, batch, sizeof batch[0] * (size_t)nb);
                g_ev.n += nb;
                nb = 0;
            }
        }
        LeaveCriticalSection(&g_ecs);
        for (int i = 0; i < nb; ++i) free(batch[i].message);   /* superseded: drop */
        total += (int)got;
        if (total >= j->max) { truncated = true; break; }
    }
    EnterCriticalSection(&g_ecs);
    if (still_current(j->gen)) {
        g_ev.busy = false;
        g_ev.truncated = truncated;
    }
    LeaveCriticalSection(&g_ecs);
    if (ctx) EvtClose(ctx);
    EvtClose(q);
    free(j);
    return 0;
}

static void demo_events(const char* channel, unsigned mask, int hours)
{
    static const struct { uint8_t lv; uint32_t id; const char* prov; const char* task; const char* msg; } E[] = {
        { 4, 7036, "Service Control Manager", "None", "The Windows Update service entered the running state." },
        { 4, 7040, "Service Control Manager", "None", "The start type of the Background Intelligent Transfer Service service was changed from auto start to demand start." },
        { 3, 10016, "Microsoft-Windows-DistributedCOM", "None", "The application-specific permission settings do not grant Local Activation permission for the COM Server application to the user DEMO-PC\\demo." },
        { 4, 37, "Microsoft-Windows-Time-Service", "None", "The time provider NtpClient is currently receiving valid time data from time.windows.com." },
        { 2, 1000, "Application Error", "Application Crashing Events", "Faulting application name: updater.exe, version: 2.4.1.0\nFaulting module name: ntdll.dll\nException code: 0xc0000005" },
        { 4, 1001, "Windows Error Reporting", "None", "Fault bucket 2318881044, type 4\nEvent Name: APPCRASH\nResponse: Not available" },
        { 4, 16384, "Microsoft-Windows-Security-SPP", "None", "Successfully scheduled Software Protection service for re-start." },
        { 3, 1014, "Microsoft-Windows-DNS-Client", "None", "Name resolution for the name updates.northwind.example timed out after none of the configured DNS servers responded." },
        { 4, 6013, "EventLog", "None", "The system uptime is 36214 seconds." },
        { 4, 7045, "Service Control Manager", "None", "A service was installed in the system.\n\nService Name: Northwind Update Service\nService Start Type: demand start" },
        { 1, 41, "Microsoft-Windows-Kernel-Power", "(63)", "The system has rebooted without cleanly shutting down first. This error could be caused if the system stopped responding, crashed, or lost power unexpectedly." },
        { 4, 1, "Microsoft-Windows-Power-Troubleshooter", "None", "The system has returned from a low power state.\n\nSleep Time: 08:12:44\nWake Time: 08:31:02" },
        { 3, 129, "storahci", "None", "Reset to device, \\Device\\RaidPort0, was issued." },
        { 4, 98, "Microsoft-Windows-Ntfs", "None", "Volume C: (\\Device\\HarddiskVolume3) is healthy. No action is needed." },
    };
    const int N = (int)(sizeof E / sizeof E[0]);
    int64_t now = 0;
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    now = (int64_t)(((((uint64_t)f.dwHighDateTime) << 32 | f.dwLowDateTime) - 116444736000000000ULL) / 10000ULL);
    int64_t cutoff = hours > 0 ? now - (int64_t)hours * 3600000LL : 0;
    EventRec* recs = calloc(160, sizeof *recs);
    int n = 0;
    uint32_t seed = 2166136261u;
    for (const char* p = channel; *p; ++p) seed = (seed ^ (uint8_t)*p) * 16777619u;
    for (int k = 0; k < 160 && recs; ++k) {
        seed = seed * 1664525u + 1013904223u;
        int i = (int)((seed >> 8) % (uint32_t)N);
        int64_t t = now - (int64_t)k * 9 * 60000LL - (int64_t)((seed >> 4) % 400000);
        if (t < cutoff) break;
        if (!((mask >> (E[i].lv > 5 ? 4 : E[i].lv)) & 1)) continue;
        EventRec* r = &recs[n++];
        r->record_id = 900000 - (uint64_t)k;
        r->time_ms = t;
        r->event_id = E[i].id;
        r->level = E[i].lv;
        r->pid = 4;
        r->tid = 112;
        snprintf(r->provider, sizeof r->provider, "%s", E[i].prov);
        snprintf(r->task, sizeof r->task, "%s", E[i].task);
        snprintf(r->computer, sizeof r->computer, "DEMO-PC");
        r->message = _strdup(E[i].msg);
    }
    EnterCriticalSection(&g_ecs);
    InterlockedIncrement(&g_gen);
    for (int i = 0; i < g_ev.n; ++i) free(g_ev.recs[i].message);
    free(g_ev.recs);
    g_ev.recs = recs;
    g_ev.n = n;
    g_ev.busy = false;
    g_ev.truncated = false;
    g_ev.error[0] = 0;
    snprintf(g_ev.channel, sizeof g_ev.channel, "%s", channel);
    LeaveCriticalSection(&g_ecs);
}

void sys_events_query(const char* channel, unsigned level_mask, int hours, int max_rows)
{
    ensure_init();
    if (sys_is_demo()) { demo_events(channel, level_mask, hours); return; }
    QueryJob* j = calloc(1, sizeof *j);
    if (!j) return;
    MultiByteToWideChar(CP_UTF8, 0, channel, -1, j->channel, 160);
    j->mask = level_mask;
    j->hours = hours;
    j->max = max_rows > 0 ? max_rows : 2000;
    EnterCriticalSection(&g_ecs);
    j->gen = InterlockedIncrement(&g_gen);
    for (int i = 0; i < g_ev.n; ++i) free(g_ev.recs[i].message);
    free(g_ev.recs);
    g_ev.recs = NULL;
    g_ev.n = 0;
    g_ev.busy = true;
    g_ev.truncated = false;
    g_ev.error[0] = 0;
    snprintf(g_ev.channel, sizeof g_ev.channel, "%s", channel);
    LeaveCriticalSection(&g_ecs);
    HANDLE t = CreateThread(NULL, 0, query_thread, j, 0, NULL);
    if (t) CloseHandle(t); else free(j);
}

/* ---- channel list ------------------------------------------------------------------- */

uint64_t sys_event_log_count(const char* channel)
{
    if (sys_is_demo()) {
        uint32_t h = 7;
        for (const char* p = channel; *p; ++p) h = h * 31 + (uint8_t)*p;
        return 400 + h % 30000;
    }
    wchar_t w[160];
    MultiByteToWideChar(CP_UTF8, 0, channel, -1, w, 160);
    EVT_HANDLE log = EvtOpenLog(NULL, w, EvtOpenChannelPath);
    if (!log) return 0;
    EVT_VARIANT v;
    DWORD used = 0;
    uint64_t n = 0;
    if (EvtGetLogInfo(log, EvtLogNumberOfLogRecords, sizeof v, &v, &used) && v.Type == EvtVarTypeUInt64) n = v.UInt64Val;
    EvtClose(log);
    return n;
}

/* Sort by the name the page shows, which drops the "Microsoft-Windows-" prefix. */
static const char* display_name(const char* s)
{
    return strncmp(s, "Microsoft-Windows-", 18) ? s : s + 18;
}

static int chan_cmp(const void* a, const void* b)
{
    return _stricmp(display_name(((const EventChannel*)a)->name), display_name(((const EventChannel*)b)->name));
}

static DWORD WINAPI channels_thread(void* arg)
{
    (void)arg;
    EVT_HANDLE en = EvtOpenChannelEnum(NULL, 0);
    EventChannel* list = NULL;
    int n = 0, cap = 0;
    if (en) {
        wchar_t path[512];
        DWORD used = 0;
        while (EvtNextChannelPath(en, 512, path, &used)) {
            if (!wcscmp(path, L"Application") || !wcscmp(path, L"System") || !wcscmp(path, L"Security") ||
                !wcscmp(path, L"Setup") || !wcscmp(path, L"ForwardedEvents"))
                continue;
            char name[160];
            w2u(path, name, sizeof name);
            uint64_t recs = sys_event_log_count(name);
            if (!recs) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                EventChannel* g = realloc(list, sizeof *list * (size_t)cap);
                if (!g) break;
                list = g;
            }
            snprintf(list[n].name, sizeof list[n].name, "%s", name);
            list[n].records = recs;
            n++;
        }
        EvtClose(en);
    }
    if (n) qsort(list, (size_t)n, sizeof *list, chan_cmp);
    EnterCriticalSection(&g_ecs);
    free(g_ev.channels);
    g_ev.channels = list;
    g_ev.nchannels = n;
    g_ev.channels_busy = false;
    LeaveCriticalSection(&g_ecs);
    return 0;
}

void sys_events_list_channels(void)
{
    ensure_init();
    if (sys_is_demo()) {
        /* a stock Windows install's logs, not this machine's */
        static const char* const names[] = {
            "Microsoft-Windows-AppReadiness/Admin", "Microsoft-Windows-Bits-Client/Operational",
            "Microsoft-Windows-Diagnostics-Performance/Operational", "Microsoft-Windows-GroupPolicy/Operational",
            "Microsoft-Windows-Kernel-PnP/Configuration", "Microsoft-Windows-NetworkProfile/Operational",
            "Microsoft-Windows-PowerShell/Operational", "Microsoft-Windows-PrintService/Admin",
            "Microsoft-Windows-StateRepository/Operational", "Microsoft-Windows-Store/Operational",
            "Microsoft-Windows-TaskScheduler/Maintenance", "Microsoft-Windows-Time-Service/Operational",
            "Microsoft-Windows-WindowsUpdateClient/Operational", "Microsoft-Windows-Winlogon/Operational",
            "Windows PowerShell",
        };
        const int n = (int)(sizeof names / sizeof names[0]);
        EventChannel* list = calloc((size_t)n, sizeof *list);
        for (int i = 0; list && i < n; ++i) {
            snprintf(list[i].name, sizeof list[i].name, "%s", names[i]);
            list[i].records = sys_event_log_count(names[i]);
        }
        EnterCriticalSection(&g_ecs);
        free(g_ev.channels);
        g_ev.channels = list;
        g_ev.nchannels = list ? n : 0;
        g_ev.channels_busy = false;
        LeaveCriticalSection(&g_ecs);
        return;
    }
    EnterCriticalSection(&g_ecs);
    bool busy = g_ev.channels_busy;
    g_ev.channels_busy = true;
    LeaveCriticalSection(&g_ecs);
    if (busy) return;
    HANDLE t = CreateThread(NULL, 0, channels_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---- XML for one event -------------------------------------------------------------- */

bool sys_event_xml(const char* channel, uint64_t record_id, char* out, int n)
{
    out[0] = 0;
    wchar_t ch[160], q[96];
    MultiByteToWideChar(CP_UTF8, 0, channel, -1, ch, 160);
    _snwprintf(q, 96, L"*[System[EventRecordID=%llu]]", (unsigned long long)record_id);
    EVT_HANDLE h = EvtQuery(NULL, ch, q, EvtQueryChannelPath);
    if (!h) return false;
    EVT_HANDLE ev = NULL;
    DWORD got = 0;
    bool ok = false;
    if (EvtNext(h, 1, &ev, 2000, 0, &got) && got) {
        DWORD used = 0, props = 0;
        EvtRender(NULL, ev, EvtRenderEventXml, 0, NULL, &used, &props);
        wchar_t* xml = used ? malloc(used + sizeof(wchar_t)) : NULL;
        if (xml && EvtRender(NULL, ev, EvtRenderEventXml, used, xml, &used, &props)) {
            /* one element per line reads far better than the single line Windows returns */
            size_t len = wcslen(xml);
            wchar_t* pretty = malloc(sizeof(wchar_t) * (len * 2 + 1));
            if (pretty) {
                size_t o = 0;
                for (size_t i = 0; i < len; ++i) {
                    pretty[o++] = xml[i];
                    if (xml[i] == L'>' && i + 1 < len && xml[i + 1] == L'<') pretty[o++] = L'\n';
                }
                pretty[o] = 0;
                w2u(pretty, out, n);
                free(pretty);
                ok = true;
            }
        }
        free(xml);
        EvtClose(ev);
    }
    EvtClose(h);
    return ok;
}
