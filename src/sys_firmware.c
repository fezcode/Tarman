/* Read-only firmware view for the Firmware page: SMBIOS tables (BIOS,
 * system, baseboard, chassis, processor socket, memory slots), UEFI global
 * variables (boot order, Secure Boot, setup mode -- these need the
 * SeSystemEnvironment privilege, i.e. an elevated Tarman), TPM, and the
 * virtualization / VBS state Windows reports.
 *
 * Vendor setup options (XMP, fan curves, ...) live in proprietary binary
 * variables whose layout is board-specific, so they are not decoded. */

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <tbs.h>
#include <cpuid.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

static const wchar_t* const EFI_GLOBAL = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

static void cpy(char* dst, size_t n, const char* src)
{
    snprintf(dst, n, "%s", src ? src : "");
    /* SMBIOS strings often carry trailing blanks or OEM placeholders */
    size_t l = strlen(dst);
    while (l && (dst[l - 1] == ' ')) dst[--l] = 0;
    if (!strcmp(dst, "To Be Filled By O.E.M.") || !strcmp(dst, "Default string") ||
        !strcmp(dst, "System Product Name") || !strcmp(dst, "System manufacturer") ||
        !strcmp(dst, "System Version") || !strcmp(dst, "System Serial Number"))
        snprintf(dst, n, "%s (OEM placeholder)", src);
}

/* String #idx of the structure at p (formatted length hlen), bounded by end. */
static const char* smb_str(const uint8_t* p, uint8_t hlen, uint8_t idx, const uint8_t* end)
{
    if (!idx) return "";
    const char* s = (const char*)p + hlen;
    while (--idx) {
        size_t l = strnlen(s, (size_t)(end - (const uint8_t*)s));
        s += l + 1;
        if ((const uint8_t*)s >= end || !*s) return "";
    }
    return s;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static const char* chassis_name(int t)
{
    switch (t) {
    case 3: return "Desktop"; case 4: return "Low-profile desktop"; case 5: return "Pizza box";
    case 6: return "Mini tower"; case 7: return "Tower"; case 8: return "Portable"; case 9: return "Laptop";
    case 10: return "Notebook"; case 11: return "Handheld"; case 13: return "All-in-one";
    case 14: return "Sub-notebook"; case 15: return "Space-saving"; case 17: return "Server";
    case 23: return "Rack mount"; case 24: return "Sealed-case PC"; case 30: return "Tablet";
    case 31: return "Convertible"; case 32: return "Detachable"; case 35: return "Mini PC"; case 36: return "Stick PC";
    }
    return "Other";
}

static const char* mem_type(int t)
{
    switch (t) {
    case 0x12: return "DDR"; case 0x13: return "DDR2"; case 0x18: return "DDR3"; case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR"; case 0x1C: return "LPDDR2"; case 0x1D: return "LPDDR3"; case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5"; case 0x23: return "LPDDR5";
    }
    return "";
}

static void parse_smbios(FirmwareInfo* fw)
{
    UINT sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (!sz) return;
    uint8_t* buf = malloc(sz);
    if (!buf) return;
    if (GetSystemFirmwareTable('RSMB', 0, buf, sz) != sz) { free(buf); return; }
    uint32_t tlen = rd32(buf + 4);
    uint8_t* p = buf + 8;
    uint8_t* end = p + tlen;
    if (end > buf + sz) end = buf + sz;
    while (p + 4 <= end) {
        uint8_t type = p[0], L = p[1];
        if (L < 4 || p + L > end || type == 127) break;
#define S(off) smb_str(p, L, (off) < L ? p[off] : 0, end)
        switch (type) {
        case 0:
            cpy(fw->bios_vendor, sizeof fw->bios_vendor, S(0x04));
            cpy(fw->bios_version, sizeof fw->bios_version, S(0x05));
            cpy(fw->bios_date, sizeof fw->bios_date, S(0x08));
            if (L > 0x09) fw->rom_kb = 64u * (p[0x09] + 1u);
            if (L >= 0x1A && p[0x09] == 0xFF) {
                uint16_t e = rd16(p + 0x18);
                fw->rom_kb = (e & 0x3FFF) * ((e >> 14) == 1 ? 1024u * 1024u : 1024u);
            }
            if (L >= 0x12) {
                uint64_t ch = (uint64_t)rd32(p + 0x0A) | ((uint64_t)rd32(p + 0x0E) << 32);
                fw->ch_flash = (ch >> 11) & 1;
                fw->ch_shadow = (ch >> 12) & 1;
                fw->ch_boot_cd = (ch >> 15) & 1;
                fw->ch_select_boot = (ch >> 16) & 1;
            }
            if (L > 0x12) { fw->ch_acpi = p[0x12] & 1; fw->ch_usb_legacy = (p[0x12] >> 1) & 1; }
            if (L > 0x13) { fw->ch_uefi = (p[0x13] >> 3) & 1; fw->ch_vm = (p[0x13] >> 4) & 1; }
            if (L > 0x15 && p[0x14] != 0xFF) snprintf(fw->bios_release, sizeof fw->bios_release, "%u.%u", p[0x14], p[0x15]);
            if (L > 0x17 && p[0x16] != 0xFF) snprintf(fw->ec_release, sizeof fw->ec_release, "%u.%u", p[0x16], p[0x17]);
            break;
        case 1:
            cpy(fw->sys_manufacturer, sizeof fw->sys_manufacturer, S(0x04));
            cpy(fw->sys_product, sizeof fw->sys_product, S(0x05));
            cpy(fw->sys_version, sizeof fw->sys_version, S(0x06));
            cpy(fw->sys_serial, sizeof fw->sys_serial, S(0x07));
            if (L >= 0x18) {
                const uint8_t* u = p + 0x08;
                snprintf(fw->sys_uuid, sizeof fw->sys_uuid,
                         "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                         u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            }
            if (L > 0x18) {
                static const char* w[] = { "Reserved", "Other", "Unknown", "APM timer", "Modem ring",
                                           "LAN remote", "Power switch", "PCI PME#", "AC power restored" };
                snprintf(fw->wake, sizeof fw->wake, "%s", p[0x18] < 9 ? w[p[0x18]] : "Other");
            }
            if (L > 0x1A) {
                cpy(fw->sys_sku, sizeof fw->sys_sku, S(0x19));
                cpy(fw->sys_family, sizeof fw->sys_family, S(0x1A));
            }
            break;
        case 2:
            cpy(fw->board_manufacturer, sizeof fw->board_manufacturer, S(0x04));
            cpy(fw->board_product, sizeof fw->board_product, S(0x05));
            cpy(fw->board_version, sizeof fw->board_version, S(0x06));
            cpy(fw->board_serial, sizeof fw->board_serial, S(0x07));
            break;
        case 3:
            cpy(fw->chassis_manufacturer, sizeof fw->chassis_manufacturer, S(0x04));
            if (L > 0x05) snprintf(fw->chassis_type, sizeof fw->chassis_type, "%s", chassis_name(p[0x05] & 0x7F));
            break;
        case 4:
            if (fw->cpu_socket[0]) break;     /* first socket only */
            cpy(fw->cpu_socket, sizeof fw->cpu_socket, S(0x04));
            cpy(fw->cpu_manufacturer, sizeof fw->cpu_manufacturer, S(0x07));
            cpy(fw->cpu_version, sizeof fw->cpu_version, S(0x10));
            if (L > 0x11) {
                uint8_t v = p[0x11];
                fw->cpu_voltage = (v & 0x80) ? (v & 0x7F) / 10.0f : (v & 1) ? 5.0f : (v & 2) ? 3.3f : (v & 4) ? 2.9f : 0;
            }
            if (L > 0x17) {
                fw->cpu_ext_clock = rd16(p + 0x12);
                fw->cpu_max_mhz = rd16(p + 0x14);
                fw->cpu_cur_mhz = rd16(p + 0x16);
            }
            if (L > 0x25) { fw->cpu_cores = p[0x23]; fw->cpu_threads = p[0x25]; }
            break;
        case 16:
            if (L > 0x0E) {
                uint32_t kb = rd32(p + 0x07);
                fw->mem_max_capacity = kb == 0x80000000u && L >= 0x17
                    ? ((uint64_t)rd32(p + 0x0F) | ((uint64_t)rd32(p + 0x13) << 32))
                    : (uint64_t)kb * 1024;
                fw->mem_array_slots += rd16(p + 0x0D);
                static const char* e[] = { "", "Other", "Unknown", "None", "Parity", "Single-bit ECC", "Multi-bit ECC", "CRC" };
                snprintf(fw->mem_ecc, sizeof fw->mem_ecc, "%s", p[0x06] < 8 ? e[p[0x06]] : "");
            }
            break;
        case 17:
            if (L >= 0x15 && fw->nslots < 16) {
                MemSlot* s = &fw->slots[fw->nslots++];
                memset(s, 0, sizeof *s);
                uint16_t size = rd16(p + 0x0C);
                if (size == 0x7FFF && L >= 0x20) s->size = (uint64_t)(rd32(p + 0x1C) & 0x7FFFFFFF) << 20;
                else if (size != 0xFFFF) s->size = (size & 0x8000) ? (uint64_t)(size & 0x7FFF) << 10 : (uint64_t)size << 20;
                static const char* ff[] = { "", "Other", "Unknown", "SIMM", "SIP", "Chip", "DIP", "ZIP",
                                            "Card", "DIMM", "TSOP", "Row of chips", "RIMM", "SODIMM" };
                snprintf(s->form, sizeof s->form, "%s", p[0x0E] < 14 ? ff[p[0x0E]] : "Other");
                cpy(s->locator, sizeof s->locator, S(0x10));
                cpy(s->bank, sizeof s->bank, S(0x11));
                snprintf(s->type, sizeof s->type, "%s", mem_type(p[0x12]));
                if (L > 0x16) s->speed = rd16(p + 0x15);
                if (L > 0x1A) {
                    cpy(s->manufacturer, sizeof s->manufacturer, S(0x17));
                    cpy(s->serial, sizeof s->serial, S(0x18));
                    cpy(s->part, sizeof s->part, S(0x1A));
                }
                if (L > 0x21) s->cfg_speed = rd16(p + 0x20);
                if (L > 0x27) s->voltage_mv = rd16(p + 0x26);
            }
            break;
        }
#undef S
        uint8_t* s = p + L;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
        p = s + 2;
    }
    free(buf);
}

static int reg_dword(const wchar_t* key, const wchar_t* val)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, val, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) return -1;
    return (int)v;
}

static void enable_privilege(const wchar_t* name)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp = { 1 };
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, NULL, NULL);
    }
    CloseHandle(tok);
}

static int efi_u8(const wchar_t* name)
{
    uint8_t v = 0;
    return GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL, &v, 1) ? v : -1;
}

static void read_uefi_vars(FirmwareInfo* fw)
{
    enable_privilege(SE_SYSTEM_ENVIRONMENT_NAME);
    uint16_t order[64];
    DWORD n = GetFirmwareEnvironmentVariableW(L"BootOrder", EFI_GLOBAL, order, sizeof order);
    if (!n) {
        DWORD e = GetLastError();
        if (e == ERROR_PRIVILEGE_NOT_HELD)
            snprintf(fw->vars_err, sizeof fw->vars_err, "Run Tarman as administrator to read UEFI variables.");
        else if (e == ERROR_INVALID_FUNCTION)
            snprintf(fw->vars_err, sizeof fw->vars_err, "This system booted in legacy BIOS mode; there are no UEFI variables.");
        else
            snprintf(fw->vars_err, sizeof fw->vars_err, "UEFI variables are not readable (error %lu).", (unsigned long)e);
        return;
    }
    fw->vars_ok = true;
    uint16_t cur = 0;
    if (GetFirmwareEnvironmentVariableW(L"BootCurrent", EFI_GLOBAL, &cur, sizeof cur)) fw->boot_current = cur;
    uint16_t to = 0;
    fw->boot_timeout = GetFirmwareEnvironmentVariableW(L"Timeout", EFI_GLOBAL, &to, sizeof to) ? to : -1;
    char lang[16] = "";
    if (GetFirmwareEnvironmentVariableW(L"PlatformLang", EFI_GLOBAL, lang, sizeof lang - 1))
        snprintf(fw->platform_lang, sizeof fw->platform_lang, "%s", lang);
    uint64_t osi = 0;
    if (GetFirmwareEnvironmentVariableW(L"OsIndicationsSupported", EFI_GLOBAL, &osi, sizeof osi))
        fw->fw_ui_supported = osi & 1;
    fw->secure_boot = efi_u8(L"SecureBoot");
    fw->setup_mode = efi_u8(L"SetupMode");
    fw->audit_mode = efi_u8(L"AuditMode");
    fw->deployed_mode = efi_u8(L"DeployedMode");

    for (DWORD i = 0; i < n / 2 && fw->nboot < 24; ++i) {
        wchar_t name[16];
        _snwprintf(name, 16, L"Boot%04X", order[i]);
        uint8_t opt[2048];
        DWORD got = GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL, opt, sizeof opt);
        BootEntry* b = &fw->boot[fw->nboot++];
        memset(b, 0, sizeof *b);
        b->id = order[i];
        b->current = order[i] == fw->boot_current;
        if (got >= 8) {
            b->active = rd32(opt) & 1;
            /* EFI_LOAD_OPTION: UINT32 Attributes, UINT16 FilePathListLength, CHAR16 Description[] */
            const wchar_t* d = (const wchar_t*)(opt + 6);
            size_t maxc = (got - 6) / 2;
            size_t len = wcsnlen(d, maxc);
            WideCharToMultiByte(CP_UTF8, 0, d, (int)len, b->name, sizeof b->name - 1, NULL, NULL);
        } else {
            snprintf(b->name, sizeof b->name, "(unreadable)");
        }
    }
}

void sys_firmware_read(FirmwareInfo* fw)
{
    memset(fw, 0, sizeof *fw);
    fw->secure_boot = fw->setup_mode = fw->audit_mode = fw->deployed_mode = -1;
    fw->boot_timeout = -1;
    fw->tpm_version = -1;

    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    GetFirmwareType(&ft);
    snprintf(fw->fw_type, sizeof fw->fw_type, "%s",
             ft == FirmwareTypeUefi ? "UEFI" : ft == FirmwareTypeBios ? "Legacy BIOS" : "Unknown");

    parse_smbios(fw);
    if (ft == FirmwareTypeUefi) read_uefi_vars(fw);
    else snprintf(fw->vars_err, sizeof fw->vars_err, "Legacy BIOS boot: no UEFI variables.");

    /* Secure Boot is also mirrored in the registry, readable without admin */
    if (fw->secure_boot < 0) {
        int sb = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled");
        if (sb >= 0) fw->secure_boot = sb;
    }

    TPM_DEVICE_INFO ti = { 0 };
    if (Tbsi_GetDeviceInfo(sizeof ti, &ti) == TBS_SUCCESS) {
        fw->tpm_version = ti.tpmVersion == TPM_VERSION_20 ? 2 : ti.tpmVersion == TPM_VERSION_12 ? 1 : 0;
        static const char* ifs[] = { "", "TIS", "TrustZone", "Hardware", "Emulator", "SPB" };
        snprintf(fw->tpm_iface, sizeof fw->tpm_iface, "%s",
                 ti.tpmInterfaceType < 6 ? ifs[ti.tpmInterfaceType] : "");
    } else {
        fw->tpm_version = 0;
    }

    fw->virt_firmware = IsProcessorFeaturePresent(21 /* PF_VIRT_FIRMWARE_ENABLED */);
    fw->nx     = IsProcessorFeaturePresent(12 /* PF_NX_ENABLED */);
    fw->sse42  = IsProcessorFeaturePresent(38 /* PF_SSE4_2_INSTRUCTIONS_AVAILABLE */);
    fw->avx2   = IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */);
    fw->avx512 = IsProcessorFeaturePresent(41 /* PF_AVX512F_INSTRUCTIONS_AVAILABLE */);
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c >> 31) & 1) {
        fw->hypervisor = true;
        __cpuid(0x40000000, a, b, c, d);
        memcpy(fw->hypervisor_vendor, &b, 4);
        memcpy(fw->hypervisor_vendor + 4, &c, 4);
        memcpy(fw->hypervisor_vendor + 8, &d, 4);
        fw->hypervisor_vendor[12] = 0;
    }
    fw->vbs_cfg = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity") == 1;
    fw->hvci_cfg = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                             L"Enabled") == 1;

    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    fw->read_ms = (int64_t)(((((uint64_t)f.dwHighDateTime) << 32 | f.dwLowDateTime) - 116444736000000000ULL) / 10000ULL);
    fw->ok = true;
}
