/* Firmware page: a read-only look at BIOS/UEFI, read once each time the tab
 * is opened (and on Refresh) rather than polled. Cards flow into two
 * columns, shortest column first. */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define MAX_ROWS_PER_CARD 32

typedef struct {
    const char* k;
    char        v[200];
    Color       badge;      /* .a != 0: draw the value as a badge */
    bool        dim;
} KV;

typedef struct {
    const char* title;
    int         icon;
    KV          rows[MAX_ROWS_PER_CARD];
    int         n;
    const char* note;       /* optional muted paragraph under the rows */
    bool        admin_button;
} FwCard;

static FwCard g_cards[8];
static int    g_ncards;
static float  g_scroll;

static FwCard* card_new(const char* title, int icon)
{
    FwCard* c = &g_cards[g_ncards++];
    memset(c, 0, sizeof *c);
    c->title = title;
    c->icon = icon;
    return c;
}

static KV* row(FwCard* c, const char* k, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static KV* row(FwCard* c, const char* k, const char* fmt, ...)
{
    if (c->n >= MAX_ROWS_PER_CARD) return &c->rows[MAX_ROWS_PER_CARD - 1];
    KV* r = &c->rows[c->n++];
    memset(r, 0, sizeof *r);
    r->k = k;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->v, sizeof r->v, fmt, ap);
    va_end(ap);
    if (!r->v[0]) { snprintf(r->v, sizeof r->v, "\xE2\x80\x94"); r->dim = true; }
    return r;
}

static void badge_row(FwCard* c, const char* k, const char* v, Color col)
{
    KV* r = row(c, k, "%s", v);
    r->badge = col;
}

static void join(char* out, size_t n, const char* const* parts, const bool* on, int count)
{
    out[0] = 0;
    for (int i = 0; i < count; ++i) {
        if (!on[i]) continue;
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", l ? "  \xC2\xB7  " : "", parts[i]);
    }
}

static void build_cards(const FirmwareInfo* fw)
{
    g_ncards = 0;
    char b[200];

    FwCard* c = card_new("BIOS / UEFI", IC_CHIP);
    badge_row(c, "Boot mode", fw->fw_type, strcmp(fw->fw_type, "UEFI") ? T.warn : T.good);
    row(c, "Vendor", "%s", fw->bios_vendor);
    row(c, "Version", "%s", fw->bios_version);
    row(c, "Release date", "%s", fw->bios_date);
    row(c, "BIOS revision", "%s", fw->bios_release);
    row(c, "EC firmware", "%s", fw->ec_release);
    if (fw->rom_kb >= 1024) row(c, "ROM size", "%u MB", fw->rom_kb / 1024);
    else row(c, "ROM size", "%u KB", fw->rom_kb);
    static const char* caps[] = { "UEFI", "ACPI", "USB legacy", "Flash upgradeable", "Boot from CD",
                                  "Selectable boot", "Shadowing", "Virtual machine" };
    bool on[] = { fw->ch_uefi, fw->ch_acpi, fw->ch_usb_legacy, fw->ch_flash, fw->ch_boot_cd,
                  fw->ch_select_boot, fw->ch_shadow, fw->ch_vm };
    join(b, sizeof b, caps, on, 8);
    row(c, "Capabilities", "%s", b);

    c = card_new("Security", IC_SHIELD);
    badge_row(c, "Secure Boot", fw->secure_boot < 0 ? "Unknown" : fw->secure_boot ? "On" : "Off",
              fw->secure_boot < 0 ? T.faint : fw->secure_boot ? T.good : T.warn);
    if (fw->setup_mode >= 0)
        row(c, "Key mode", "%s%s%s", fw->setup_mode ? "Setup mode (no platform key)" : "User mode",
            fw->audit_mode == 1 ? "  \xC2\xB7  audit" : "", fw->deployed_mode == 1 ? "  \xC2\xB7  deployed" : "");
    if (fw->tpm_version > 0) {
        snprintf(b, sizeof b, "TPM %s%s%s", fw->tpm_version == 2 ? "2.0" : "1.2", fw->tpm_iface[0] ? "  \xC2\xB7  " : "", fw->tpm_iface);
        badge_row(c, "TPM", b, T.good);
    } else {
        badge_row(c, "TPM", fw->tpm_version == 0 ? "Not found" : "Unknown", T.warn);
    }
    badge_row(c, "Virtualization", fw->virt_firmware ? "Enabled in firmware" : "Disabled in firmware",
              fw->virt_firmware ? T.good : T.faint);
    row(c, "Hypervisor", "%s", fw->hypervisor ? (strstr(fw->hypervisor_vendor, "Microsoft") ? "Hyper-V (Microsoft Hv)" : fw->hypervisor_vendor) : "Not running");
    row(c, "DEP / NX", "%s", fw->nx ? "Enabled" : "Disabled");
    row(c, "VBS", "%s", fw->vbs_cfg ? "Configured on" : "Not configured");
    row(c, "Memory integrity", "%s", fw->hvci_cfg ? "On (HVCI)" : "Off");

    c = card_new("System", IC_MONITOR);
    row(c, "Manufacturer", "%s", fw->sys_manufacturer);
    row(c, "Product", "%s", fw->sys_product);
    row(c, "Version", "%s", fw->sys_version);
    row(c, "Family", "%s", fw->sys_family);
    row(c, "SKU", "%s", fw->sys_sku);
    row(c, "Serial number", "%s", fw->sys_serial);
    row(c, "UUID", "%s", fw->sys_uuid);
    row(c, "Wake-up type", "%s", fw->wake);
    row(c, "Chassis", "%s%s%s", fw->chassis_type, fw->chassis_manufacturer[0] ? "  \xC2\xB7  " : "", fw->chassis_manufacturer);

    c = card_new("Motherboard", IC_CHIP);
    row(c, "Manufacturer", "%s", fw->board_manufacturer);
    row(c, "Product", "%s", fw->board_product);
    row(c, "Version", "%s", fw->board_version);
    row(c, "Serial number", "%s", fw->board_serial);

    c = card_new("Processor (SMBIOS)", IC_CHIP);
    row(c, "Model", "%s", fw->cpu_version);
    row(c, "Manufacturer", "%s", fw->cpu_manufacturer);
    row(c, "Socket", "%s", fw->cpu_socket);
    if (fw->cpu_max_mhz) row(c, "Max speed", "%u MHz", fw->cpu_max_mhz);
    if (fw->cpu_cur_mhz) row(c, "Boot speed", "%u MHz", fw->cpu_cur_mhz);
    if (fw->cpu_ext_clock) row(c, "External clock", "%u MHz", fw->cpu_ext_clock);
    if (fw->cpu_voltage > 0) row(c, "Voltage", "%.1f V", fw->cpu_voltage);
    if (fw->cpu_cores) row(c, "Cores / threads", "%d / %d", fw->cpu_cores, fw->cpu_threads);
    static const char* isa[] = { "SSE4.2", "AVX2", "AVX-512" };
    bool ion[] = { fw->sse42, fw->avx2, fw->avx512 };
    join(b, sizeof b, isa, ion, 3);
    row(c, "Instruction sets", "%s", b);

    c = card_new("Boot (UEFI variables)", IC_POWER);
    if (!fw->vars_ok) {
        c->note = fw->vars_err;
        c->admin_button = !app.st->elevated && strstr(fw->vars_err, "administrator");
        if (fw->secure_boot >= 0) row(c, "Secure Boot", "%s (from registry)", fw->secure_boot ? "On" : "Off");
    } else {
        for (int i = 0; i < fw->nboot; ++i)
            if (fw->boot[i].current) row(c, "Booted from", "%s", fw->boot[i].name);
        if (fw->boot_timeout >= 0) row(c, "Menu timeout", "%d s", fw->boot_timeout);
        row(c, "Platform language", "%s", fw->platform_lang);
        row(c, "Restart to setup", "%s", fw->fw_ui_supported ? "Supported (shutdown /r /fw)" : "Not supported");
        for (int i = 0; i < fw->nboot && c->n < MAX_ROWS_PER_CARD; ++i) {
            static char keys[24][24];
            snprintf(keys[i], sizeof keys[i], "%d.  Boot%04X", i + 1, fw->boot[i].id);
            KV* r = row(c, keys[i], "%s%s%s", fw->boot[i].name, fw->boot[i].current ? "   \xE2\x86\x90 current" : "",
                        fw->boot[i].active ? "" : "   (disabled)");
            r->dim = !fw->boot[i].active;
        }
    }

    c = card_new("Memory slots", IC_MEMORY);
    char cap[32];
    fmt_bytes((double)fw->mem_max_capacity, cap, sizeof cap);
    if (fw->mem_max_capacity) row(c, "Max capacity", "%s", cap);
    int used = 0;
    for (int i = 0; i < fw->nslots; ++i) used += fw->slots[i].size > 0;
    row(c, "Slots used", "%d of %d", used, fw->mem_array_slots ? fw->mem_array_slots : fw->nslots);
    if (fw->mem_ecc[0]) row(c, "Error correction", "%s", fw->mem_ecc);
    for (int i = 0; i < fw->nslots && c->n < MAX_ROWS_PER_CARD; ++i) {
        const MemSlot* s = &fw->slots[i];
        if (!s->size) { KV* r = row(c, s->locator, "Empty"); r->dim = true; continue; }
        char sz[32], spd[48] = "", volt[16] = "";
        fmt_bytes((double)s->size, sz, sizeof sz);
        if (s->cfg_speed && s->speed && s->cfg_speed != s->speed)
            snprintf(spd, sizeof spd, "%u MT/s (rated %u)", s->cfg_speed, s->speed);
        else if (s->speed)
            snprintf(spd, sizeof spd, "%u MT/s", s->speed);
        if (s->voltage_mv) snprintf(volt, sizeof volt, "%.2f V", s->voltage_mv / 1000.0);
        row(c, s->locator, "%s %s %s  \xC2\xB7  %s  \xC2\xB7  %s %s  %s", sz, s->type, s->form, spd, s->manufacturer, s->part, volt);
    }
}

static float card_height(const FwCard* c, float w)
{
    float h = 52 + c->n * 26;
    if (c->note) h += 44;
    if (c->admin_button) h += 44;
    (void)w;
    return h + 10;
}

static void draw_card(const FwCard* c, Rectangle r)
{
    card(r);
    Rectangle in = rect_inset(r, 18, 14);
    Rectangle h = rect_cut_top(&in, 30);
    ui_icon(h.x + 9, h.y + 12, c->icon, 15, T.accent);
    ui_text_fit((Rectangle){ h.x + 28, h.y, h.width - 28, 24 }, c->title, 14, FW_SEMI, T.text, AL_LEFT);
    rect_cut_top(&in, 6);
    float kw = fminf(150, in.width * 0.36f);
    for (int i = 0; i < c->n; ++i) {
        const KV* kv = &c->rows[i];
        Rectangle rr = rect_cut_top(&in, 26);
        bool hov = ui_hover(rr);
        if (hov) ui_rrect((Rectangle){ rr.x - 6, rr.y, rr.width + 12, rr.height }, 5, T.hover);
        ui_text_fit((Rectangle){ rr.x, rr.y, kw - 8, rr.height }, kv->k, 12.5f, FW_REG, T.dim, AL_LEFT);
        Rectangle vr = { rr.x + kw, rr.y, rr.width - kw, rr.height };
        if (kv->badge.a) ui_badge(vr.x, rr.y + rr.height / 2, kv->v, kv->badge);
        else ui_text_fit(vr, kv->v, 12.5f, FW_REG, kv->dim ? T.faint : T.text, AL_LEFT);
        if (hov) {
            if (ui_text_w(kv->v, 12.5f, FW_REG) > vr.width) {
                char tip[300];
                snprintf(tip, sizeof tip, "%s\n%s\nDouble-click to copy", kv->k, kv->v);
                ui_tooltip(tip);
            } else {
                ui_tooltip("Double-click to copy");
            }
            if (ui_dclicked(rr)) { SetClipboardText(kv->v); toast(false, "Copied %s", kv->k); }
        }
    }
    if (c->note) {
        Rectangle nr = rect_cut_top(&in, 44);
        ui_rrect((Rectangle){ nr.x, nr.y + 6, nr.width, 32 }, 6, col_alpha(T.warn, 0.10f));
        ui_icon(nr.x + 16, nr.y + 22, IC_INFO, 13, T.warn);
        ui_text_fit((Rectangle){ nr.x + 34, nr.y + 6, nr.width - 42, 32 }, c->note, 12.5f, FW_REG, T.text, AL_LEFT);
    }
    if (c->admin_button) {
        Rectangle br = rect_cut_top(&in, 44);
        float w = ui_button_w("Restart as administrator", IC_SHIELD);
        if (ui_button((Rectangle){ br.x, br.y + 8, w, 32 }, "Restart as administrator", IC_SHIELD, BTN_PRIMARY, true))
            if (sys_restart_elevated()) app.quit = true;
    }
}

void page_firmware(Rectangle r)
{
    if (!app.fw_loaded) {
        sys_firmware_read(&app.fw);
        app.fw_loaded = true;
    }
    FirmwareInfo* fw = &app.fw;
    char sub[96], clock[16];
    sys_format_clock(fw->read_ms, clock, sizeof clock);
    snprintf(sub, sizeof sub, "Read-only  \xC2\xB7  read at %s", clock);
    Rectangle tb = page_header(&r, "Firmware", sub);
    float w = ui_button_w("Refresh", IC_REFRESH);
    if (ui_button(rect_cut_right(&tb, w), "Refresh", IC_REFRESH, BTN_SUBTLE, true)) {
        sys_firmware_read(&app.fw);
        toast(false, "Firmware information re-read");
    }

    build_cards(fw);

    /* masonry: two columns when there is room */
    int cols = r.width > 980 ? 2 : 1;
    float gap = 14, cw = (r.width - 14 - (cols - 1) * gap) / cols;
    float colh[2] = { 0, 0 };
    Rectangle pos[8];
    for (int i = 0; i < g_ncards; ++i) {
        int c = cols == 2 && colh[1] < colh[0] ? 1 : 0;
        float h = card_height(&g_cards[i], cw);
        pos[i] = (Rectangle){ r.x + c * (cw + gap), r.y + colh[c], cw, h };
        colh[c] += h + gap;
    }
    float content = fmaxf(colh[0], colh[1]) + 60;
    ui_wheel(r, &g_scroll, 60);
    float maxs = fmaxf(0, content - r.height);
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;
    ui_scissor_push(r);
    for (int i = 0; i < g_ncards; ++i) {
        Rectangle p = pos[i];
        p.y -= g_scroll;
        if (p.y > r.y + r.height || p.y + p.height < r.y) continue;
        draw_card(&g_cards[i], p);
    }
    float ny = r.y + fmaxf(colh[0], colh[1]) - g_scroll + 4;
    ui_text_fit((Rectangle){ r.x, ny, r.width - 14, 22 },
                "Vendor setup options (XMP, fan curves, power limits) are stored in board-specific binary "
                "variables and can't be decoded generically.",
                12, FW_REG, T.faint, AL_LEFT);
    ui_scissor_pop();
    ui_scrollbar((Rectangle){ r.x + r.width - 8, r.y, 8, r.height }, &g_scroll, content, r.height, ui_id("fw-scroll", 0));
}
