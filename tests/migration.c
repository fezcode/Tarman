/* Run in an empty scratch directory. Include the implementation to exercise
 * the actual private migration helper without touching Windows known folders.
 * Build with -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections and the
 * same Windows system libraries as Tarman (raylib/GLFW are not needed). */
#include "../src/sys_win.c"
#include <assert.h>

static void put_file(const wchar_t* path, const char* value)
{
    FILE* f = _wfopen(path, L"wb");
    assert(f);
    assert(fwrite(value, 1, strlen(value), f) == strlen(value));
    assert(fclose(f) == 0);
}

static void check_file(const wchar_t* path, const char* value)
{
    char buf[128] = { 0 };
    FILE* f = _wfopen(path, L"rb");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    assert(!ferror(f));
    fclose(f);
    assert(n == strlen(value) && !strcmp(buf, value));
}

int main(void)
{
    assert(CreateDirectoryW(L"old", NULL));
    assert(CreateDirectoryW(L"old\\logs", NULL));
    assert(CreateDirectoryW(L"old\\nested", NULL));
    assert(CreateDirectoryW(L"settings", NULL));
    assert(CreateDirectoryW(L"logs", NULL));
    put_file(L"old\\settings.ini", "old settings");
    put_file(L"settings\\settings.ini", "current settings");
    put_file(L"settings\\settings.ini.migrated-1", "earlier backup");
    put_file(L"old\\nested\\extra.txt", "nested data");
    put_file(L"old\\logs\\today.log", "old log entries");
    put_file(L"logs\\today.log", "new log entries");
    put_file(L"old\\logs\\locked.log", "locked data");
    HANDLE locked = CreateFileW(L"old\\logs\\locked.log", GENERIC_READ, 0, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(locked != INVALID_HANDLE_VALUE);

    move_tree(L"old\\logs", L"logs", false);
    move_tree(L"old", L"settings", true);
    check_file(L"settings\\settings.ini", "current settings");
    check_file(L"settings\\settings.ini.migrated-1", "earlier backup");
    check_file(L"settings\\settings.ini.migrated-2", "old settings");
    check_file(L"settings\\nested\\extra.txt", "nested data");
    check_file(L"logs\\today.log", "new log entries");
    check_file(L"logs\\today.log.migrated-1", "old log entries");
    assert(GetFileAttributesW(L"old\\logs\\locked.log") != INVALID_FILE_ATTRIBUTES);
    assert(GetFileAttributesW(L"settings\\logs") == INVALID_FILE_ATTRIBUTES);

    CloseHandle(locked);
    move_tree(L"old\\logs", L"logs", false);
    move_tree(L"old", L"settings", true);
    check_file(L"logs\\locked.log", "locked data");
    assert(GetFileAttributesW(L"old") == INVALID_FILE_ATTRIBUTES);
    move_tree(L"old\\logs", L"logs", false);
    move_tree(L"old", L"settings", true);
    check_file(L"settings\\settings.ini", "current settings");
    check_file(L"settings\\settings.ini.migrated-2", "old settings");
    assert(GetFileAttributesW(L"settings\\settings.ini.migrated-3") == INVALID_FILE_ATTRIBUTES);
    puts("PASS: collisions, numbered backups, nested files, locked logs, retries and idempotence");
    return 0;
}
