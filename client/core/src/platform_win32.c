#if defined(_WIN32)

#include "platform.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/buf.h"
#include "vapor/util.h"

static int
known_folder(const KNOWNFOLDERID *id, char *out, size_t outsz)
{
    PWSTR wpath = NULL;
    int   n;

    if (FAILED(SHGetKnownFolderPath(id, 0, NULL, &wpath))) {
        return -1;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, (int)outsz, NULL, NULL);
    CoTaskMemFree(wpath);
    return n > 0 ? 0 : -1;
}

int
vapor_plat_data_dir(char *out, size_t outsz)
{
    char base[VAPOR_WIN_PATH];

    if (known_folder(&FOLDERID_LocalAppData, base, sizeof(base)) != 0) {
        const char *env = getenv("LOCALAPPDATA");
        if (!env || !*env) {
            return -1;
        }
        snprintf(base, sizeof(base), "%s", env);
    }
    snprintf(out, outsz, "%s\\Vapor", base);
    return 0;
}

int
vapor_plat_default_library_dir(char *out, size_t outsz)
{
    char base[VAPOR_WIN_PATH];

    /* Games go under the user profile, not Program Files: no elevation, and no
     * fighting with Windows' installer redirection. */
    if (known_folder(&FOLDERID_LocalAppData, base, sizeof(base)) != 0) {
        const char *env = getenv("LOCALAPPDATA");
        if (!env || !*env) {
            return -1;
        }
        snprintf(base, sizeof(base), "%s", env);
    }
    snprintf(out, outsz, "%s\\Vapor\\Games", base);
    return 0;
}

int
vapor_plat_mkdirs(const char *path)
{
    char   tmp[VAPOR_WIN_PATH];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (i = 0; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\\';
        }
    }

    /* Start past "C:\" so we never try to create a drive root. */
    for (i = (n > 2 && tmp[1] == ':') ? 3 : 1; i <= n; i++) {
        if (tmp[i] == '\\' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (!CreateDirectoryA(tmp, NULL)
                && GetLastError() != ERROR_ALREADY_EXISTS) {
                return -1;
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

int
vapor_plat_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int
vapor_plat_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int
vapor_plat_file_size(const char *path, uint64_t *out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        return -1;
    }
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return -1;
    }
    *out = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 0;
}

int
vapor_plat_remove_tree(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pattern[VAPOR_WIN_PATH];
    char             child[VAPOR_WIN_PATH];
    DWORD            attrs;

    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return 0; /* already gone */
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (attrs & FILE_ATTRIBUTE_READONLY) {
            SetFileAttributesA(path, attrs & ~FILE_ATTRIBUTE_READONLY);
        }
        return DeleteFileA(path) ? 0 : -1;
    }

    if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", path)
        >= sizeof(pattern)) {
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName)
            >= sizeof(child)) {
            FindClose(h);
            return -1;
        }
        if (vapor_plat_remove_tree(child) != 0) {
            FindClose(h);
            return -1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    return RemoveDirectoryA(path) ? 0 : -1;
}

int
vapor_plat_dir_size(const char *path, uint64_t *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pattern[VAPOR_WIN_PATH];
    char             child[VAPOR_WIN_PATH];

    if (!vapor_plat_is_dir(path)) {
        uint64_t sz = 0;
        if (vapor_plat_file_size(path, &sz) == 0) {
            *out += sz;
            return 0;
        }
        return -1;
    }

    if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", path)
        >= sizeof(pattern)) {
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName)
            >= sizeof(child)) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            vapor_plat_dir_size(child, out);
        } else {
            *out += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

int
vapor_plat_make_executable(const char *path)
{
    /* Windows has no execute bit; being a .exe is enough. */
    (void)path;
    return 0;
}

int
vapor_plat_chmod_matching(const char *root, const char *pattern, size_t *count)
{
    /* exec_bits only exist because zip cannot carry the Unix executable bit. */
    (void)root;
    (void)pattern;
    (void)count;
    return 0;
}

static int
skip_walk_name(const char *name)
{
    return name[0] == '.' || strcmp(name, "__MACOSX") == 0;
}

static int
walk_files(const char *root, const char *rel, vapor_plat_walk_fn fn, void *ud)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pattern[VAPOR_WIN_PATH];
    char             abs[VAPOR_WIN_PATH];
    int              rc = 0;

    if (rel[0]) {
        if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", root, rel)
            >= sizeof(abs)) {
            return -1;
        }
        vapor_plat_native_path(abs);
        if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", abs)
            >= sizeof(pattern)) {
            return -1;
        }
    } else if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", root)
               >= sizeof(pattern)) {
        return -1;
    }

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        char child_rel[VAPOR_WIN_PATH];
        char child_abs[VAPOR_WIN_PATH];

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (skip_walk_name(fd.cFileName)) {
            continue;
        }
        if ((size_t)snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                             *rel ? "/" : "", fd.cFileName)
            >= sizeof(child_rel)) {
            continue;
        }
        if ((size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", root,
                             child_rel)
            >= sizeof(child_abs)) {
            continue;
        }
        vapor_plat_native_path(child_abs);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rc = walk_files(root, child_rel, fn, ud);
            if (rc != 0) {
                break;
            }
            continue;
        }
        rc = fn(child_rel, child_abs, ud);
        if (rc != 0) {
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
}

int
vapor_plat_walk_files(const char *root, vapor_plat_walk_fn fn, void *ud)
{
    if (!root || !fn) {
        return -1;
    }
    return walk_files(root, "", fn, ud);
}

void
vapor_plat_native_path(char *path)
{
    for (; *path; path++) {
        if (*path == '/') {
            *path = '\\';
        }
    }
}

/* Rebuilds argv into a single command line with the quoting rules CommandLineToArgvW
 * uses, which is what CreateProcess expects. */
static int
build_command_line(char *const argv[], vapor_buf *out)
{
    int i;

    for (i = 0; argv[i]; i++) {
        const char *a = argv[i];
        int         needs_quotes = (*a == '\0') || strpbrk(a, " \t\"") != NULL;

        if (i > 0 && vapor_buf_appends(out, " ") != 0) {
            return -1;
        }
        if (!needs_quotes) {
            if (vapor_buf_appends(out, a) != 0) {
                return -1;
            }
            continue;
        }

        if (vapor_buf_appends(out, "\"") != 0) {
            return -1;
        }
        while (*a) {
            size_t nbs = 0;
            while (*a == '\\') {
                nbs++;
                a++;
            }
            if (*a == '\0') {
                /* Backslashes before the closing quote must be doubled. */
                size_t k;
                for (k = 0; k < nbs * 2; k++) {
                    if (vapor_buf_appends(out, "\\") != 0) { return -1; }
                }
                break;
            }
            if (*a == '"') {
                size_t k;
                for (k = 0; k < nbs * 2 + 1; k++) {
                    if (vapor_buf_appends(out, "\\") != 0) { return -1; }
                }
                if (vapor_buf_append(out, a, 1) != 0) { return -1; }
            } else {
                size_t k;
                for (k = 0; k < nbs; k++) {
                    if (vapor_buf_appends(out, "\\") != 0) { return -1; }
                }
                if (vapor_buf_append(out, a, 1) != 0) { return -1; }
            }
            a++;
        }
        if (vapor_buf_appends(out, "\"") != 0) {
            return -1;
        }
    }
    return 0;
}

int
vapor_plat_run(const char *exec, char *const argv[], const char *cwd,
               const vapor_kv *env, size_t nenv, int *out_exit)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    vapor_buf           cmd;
    DWORD               code = 0;
    size_t              i;
    int                 rc = -1;

    vapor_buf_init(&cmd);
    if (build_command_line(argv, &cmd) != 0) {
        vapor_buf_free(&cmd);
        return -1;
    }

    /* SetEnvironmentVariable mutates our own block, which CreateProcess then
     * copies to the child. Acceptable because launching is a blocking,
     * one-at-a-time operation in the CLI and GUI alike. */
    for (i = 0; i < nenv; i++) {
        if (env[i].key && env[i].value) {
            SetEnvironmentVariableA(env[i].key, env[i].value);
        }
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(exec, cmd.data, NULL, NULL, FALSE, 0, NULL,
                       (cwd && *cwd) ? cwd : NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (out_exit) {
            *out_exit = (int)code;
        }
        rc = 0;
    } else if (GetLastError() == ERROR_ELEVATION_REQUIRED) {
        vapor_buf_free(&cmd);
        return vapor_plat_run_ui(exec, cwd, NULL, out_exit);
    }

    vapor_buf_free(&cmd);
    return rc;
}

int
vapor_plat_search_path(const char *name, char *out, size_t outsz)
{
    DWORD n;

    if (!name || !*name || !out || outsz == 0) {
        return 1;
    }
    n = SearchPathA(NULL, name, NULL, (DWORD)outsz, out, NULL);
    if (n == 0 || n >= (DWORD)outsz) {
        out[0] = '\0';
        return 1;
    }
    return 0;
}

static void
trim_slash(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\\' || s[n - 1] == '/')) {
        s[--n] = '\0';
    }
}

static int
reg_query_str(HKEY key, const char *name, char *out, size_t outsz)
{
    DWORD type = 0, n = (DWORD)outsz;
    char  raw[VAPOR_WIN_PATH];

    if (outsz == 0) {
        return -1;
    }
    out[0] = '\0';
    n = sizeof(raw);
    if (RegQueryValueExA(key, name, NULL, &type, (LPBYTE)raw, &n) != ERROR_SUCCESS
        || n == 0) {
        return -1;
    }
    raw[sizeof(raw) - 1] = '\0';
    if (type == REG_EXPAND_SZ) {
        if (ExpandEnvironmentStringsA(raw, out, (DWORD)outsz) == 0) {
            return -1;
        }
    } else if (type == REG_SZ) {
        snprintf(out, outsz, "%s", raw);
    } else {
        return -1;
    }
    return 0;
}

static int
name_matches_product(const char *display, const char *name, const char *id)
{
    char slug[VAPOR_ID_MAX + 1];
    char want[VAPOR_ID_MAX + 1];

    if (!display || !display[0]) {
        return 0;
    }
    if (vapor_id_slug(display, slug, sizeof(slug)) != 0) {
        return 0;
    }
    if (id && *id && vapor_slug_match(slug, id)) {
        return 1;
    }
    if (name && *name && vapor_id_slug(name, want, sizeof(want)) == 0
        && vapor_slug_match(slug, want)) {
        return 1;
    }
    return 0;
}

static int dir_has_entries(const char *dir);

static int
scan_uninstall_root(HKEY hive, const char *sub, const char *name, const char *id,
                    char *out, size_t outsz, int require_files)
{
    HKEY  k;
    DWORD i;

    if (RegOpenKeyExA(hive, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        return 1;
    }
    for (i = 0;; i++) {
        char  child[256];
        char  path[512];
        char  display[256];
        char  loc[VAPOR_WIN_PATH];
        char  icon[VAPOR_WIN_PATH];
        DWORD childn = sizeof(child);
        HKEY  p;
        char *slash;

        if (RegEnumKeyExA(k, i, child, &childn, NULL, NULL, NULL, NULL)
            != ERROR_SUCCESS) {
            break;
        }
        if (snprintf(path, sizeof(path), "%s\\%s", sub, child) >= (int)sizeof(path)
            || RegOpenKeyExA(hive, path, 0, KEY_READ, &p) != ERROR_SUCCESS) {
            continue;
        }
        display[0] = loc[0] = icon[0] = '\0';
        (void)reg_query_str(p, "DisplayName", display, sizeof(display));
        (void)reg_query_str(p, "InstallLocation", loc, sizeof(loc));
        if (!loc[0] && reg_query_str(p, "DisplayIcon", icon, sizeof(icon)) == 0) {
            char *comma = strrchr(icon, ',');
            if (comma) {
                *comma = '\0';
            }
            snprintf(loc, sizeof(loc), "%s", icon);
            slash = strrchr(loc, '\\');
            if (slash) {
                *slash = '\0';
            }
        }
        RegCloseKey(p);
        if (!name_matches_product(display, name, id) || !loc[0]) {
            continue;
        }
        trim_slash(loc);
        if (GetFileAttributesA(loc) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (require_files && !dir_has_entries(loc)) {
            continue;
        }
        snprintf(out, outsz, "%s", loc);
        RegCloseKey(k);
        return 0;
    }
    RegCloseKey(k);
    return 1;
}

static int
dir_has_entries(const char *dir)
{
    char            pat[VAPOR_WIN_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE          h;
    int             found = 0;

    if (snprintf(pat, sizeof(pat), "%s\\*", dir) >= (int)sizeof(pat)) {
        return 0;
    }
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (fd.cFileName[0] != '.') {
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int
scan_program_files(const char *root, const char *name, const char *id,
                   char *out, size_t outsz, int require_files)
{
    char            pat[VAPOR_WIN_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE          h;

    if (!root || !root[0]) {
        return 1;
    }
    if (snprintf(pat, sizeof(pat), "%s\\*", root) >= (int)sizeof(pat)) {
        return 1;
    }
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    do {
        char            child[VAPOR_WIN_PATH];
        char            subpat[VAPOR_WIN_PATH];
        WIN32_FIND_DATAA fd2;
        HANDLE          h2;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || fd.cFileName[0] == '.') {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s\\%s", root, fd.cFileName)
            >= (int)sizeof(child)) {
            continue;
        }
        if (name_matches_product(fd.cFileName, name, id)) {
            /* InstallShield often creates the destination before copying; an
             * empty "DOOM 3" folder is not a finished install. */
            if (require_files && !dir_has_entries(child)) {
                continue;
            }
            snprintf(out, outsz, "%s", child);
            FindClose(h);
            return 0;
        }
        if (snprintf(subpat, sizeof(subpat), "%s\\*", child)
            >= (int)sizeof(subpat)) {
            continue;
        }
        h2 = FindFirstFileA(subpat, &fd2);
        if (h2 == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || fd2.cFileName[0] == '.') {
                continue;
            }
            if (name_matches_product(fd2.cFileName, name, id)) {
                char nested[VAPOR_WIN_PATH];

                if (snprintf(nested, sizeof(nested), "%s\\%s", child,
                             fd2.cFileName) >= (int)sizeof(nested)
                    || (require_files && !dir_has_entries(nested))) {
                    continue;
                }
                snprintf(out, outsz, "%s", nested);
                FindClose(h2);
                FindClose(h);
                return 0;
            }
        } while (FindNextFileA(h2, &fd2));
        FindClose(h2);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}

#define VAPOR_MAX_HELPER_PIDS 512

typedef struct {
    DWORD pids[VAPOR_MAX_HELPER_PIDS];
    int   n;
} vapor_pid_set;

static int
pid_set_has(const vapor_pid_set *s, DWORD pid)
{
    int i;

    for (i = 0; i < s->n; i++) {
        if (s->pids[i] == pid) {
            return 1;
        }
    }
    return 0;
}

/* Engines that a bootstrapper (setup.exe) commonly starts and then exits
 * while they keep running. Names only — no per-title logic. */
static int
is_installer_helper_image(const char *name)
{
    return vapor_str_eq_ci(name, "msiexec.exe")
        || vapor_str_eq_ci(name, "setup.exe")
        || vapor_str_eq_ci(name, "install.exe")
        || vapor_str_eq_ci(name, "installer.exe")
        || vapor_str_eq_ci(name, "isbew64.exe")
        || vapor_str_eq_ci(name, "isbew32.exe")
        || vapor_str_eq_ci(name, "installshield.exe")
        || vapor_str_eq_ci(name, "instmsiw.exe")
        || vapor_str_eq_ci(name, "instmsia.exe")
        || vapor_str_eq_ci(name, "idriver.exe")
        || vapor_str_eq_ci(name, "ikernel.exe")
        || vapor_str_eq_ci(name, "isrt.exe")
        || vapor_str_has_prefix(name, "issetup");
}

static void
collect_installer_helpers(vapor_pid_set *out)
{
    HANDLE         snap;
    PROCESSENTRY32 pe;

    out->n = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (is_installer_helper_image(pe.szExeFile)
                && out->n < VAPOR_MAX_HELPER_PIDS) {
                out->pids[out->n++] = pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

static int
new_installer_helpers_running(const vapor_pid_set *before, DWORD launched)
{
    vapor_pid_set now;
    int           i;

    collect_installer_helpers(&now);
    for (i = 0; i < now.n; i++) {
        if (now.pids[i] != launched && !pid_set_has(before, now.pids[i])) {
            return 1;
        }
    }
    return 0;
}

/* A bootstrapper often returns as soon as msiexec (or a second setup.exe) is
 * up. Wait until those newcomers exit so we do not scan an unfinished tree.
 * Cap the wait: a stray msiexec (Windows Update) must not block forever. */
static void
wait_new_installer_helpers(const vapor_pid_set *before, DWORD launched)
{
    int ticks;

    for (ticks = 0; ticks < 600; ticks++) {
        if (!new_installer_helpers_running(before, launched)) {
            return;
        }
        if (ticks == 0 || ticks % 15 == 14) {
            printf("  waiting for installer helper processes (%d s)\n",
                   ticks + 1);
            fflush(stdout);
        }
        Sleep(1000);
    }
    printf("  helpers still running; checking the destination anyway\n");
    fflush(stdout);
}

static int
try_sibling_bootstrapper(const char *exec, char *out, size_t outsz)
{
    char               dir[VAPOR_WIN_PATH];
    char               cand[VAPOR_WIN_PATH];
    const char        *slash;
    static const char *const names[] = {
        "setup.exe", "install.exe", "installer.exe", NULL
    };
    size_t n;
    int    i;

    if (!vapor_str_ends_with_ci(exec, ".msi")) {
        return 0;
    }
    slash = strrchr(exec, '\\');
    if (!slash) {
        slash = strrchr(exec, '/');
    }
    if (!slash) {
        n = 0;
        dir[0] = '.';
        dir[1] = '\0';
    } else {
        n = (size_t)(slash - exec);
        if (n == 0 || n >= sizeof(dir)) {
            return 0;
        }
        memcpy(dir, exec, n);
        dir[n] = '\0';
    }
    for (i = 0; names[i]; i++) {
        if (snprintf(cand, sizeof(cand), "%s\\%s", dir, names[i])
            >= (int)sizeof(cand)) {
            continue;
        }
        if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, outsz, "%s", cand);
            return 1;
        }
    }
    return 0;
}

int
vapor_plat_run_ui(const char *exec, const char *cwd, const char *extra,
                  int *out_exit)
{
    SHELLEXECUTEINFOA sei;
    char              msiexec[MAX_PATH];
    char              msiexec_params[VAPOR_WIN_PATH + 64];
    char              bootstrap[VAPOR_WIN_PATH];
    char              cwd_buf[VAPOR_WIN_PATH];
    const char       *file = exec;
    const char       *par = (extra && *extra) ? extra : NULL;
    const char       *verb = "open";
    DWORD             code = 0;
    DWORD             launched = 0;
    HRESULT           hr;
    int               uninit = 0;
    int               rc = -1;
    int               wait_helpers = 0;
    vapor_pid_set     before;

    if (!exec || !*exec) {
        return -1;
    }
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        uninit = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return -1;
    }
    if (try_sibling_bootstrapper(exec, bootstrap, sizeof(bootstrap))) {
        file = bootstrap;
        exec = bootstrap;
        if (!cwd || !*cwd) {
            const char *slash = strrchr(bootstrap, '\\');
            if (slash && (size_t)(slash - bootstrap) < sizeof(cwd_buf)) {
                memcpy(cwd_buf, bootstrap, (size_t)(slash - bootstrap));
                cwd_buf[slash - bootstrap] = '\0';
                cwd = cwd_buf;
            }
        }
        /* Caller may have built msiexec switches for the .msi; a sibling
         * setup.exe takes over, so drop those and let the wrapper run. */
        if (par && (strstr(par, "/i ") == par || strstr(par, "/i\t") == par
                    || vapor_str_has_prefix(par, "/i \""))) {
            par = NULL;
        }
    }
    wait_helpers = vapor_str_ends_with_ci(exec, ".exe")
                   || vapor_str_ends_with_ci(exec, ".msi");
    if (wait_helpers) {
        collect_installer_helpers(&before);
    } else {
        before.n = 0;
    }
    if (vapor_str_ends_with_ci(exec, ".msi")) {
        char sysdir[MAX_PATH];
        UINT n = GetSystemDirectoryA(sysdir, MAX_PATH);

        if (n == 0 || n >= MAX_PATH) {
            goto done;
        }
        if (snprintf(msiexec, sizeof(msiexec), "%s\\msiexec.exe", sysdir)
            >= (int)sizeof(msiexec)) {
            goto done;
        }
        if (!par) {
            if (snprintf(msiexec_params, sizeof(msiexec_params), "/i \"%s\"",
                         exec)
                >= (int)sizeof(msiexec_params)) {
                goto done;
            }
            par = msiexec_params;
        }
        file = msiexec;
        verb = "runas";
    }
    /* .exe uses "open" so an embedded requireAdministrator manifest can elevate
     * normally. Forcing "runas" makes some wrappers hand the inner MSI to
     * msiexec, which then errors with "you must run Setup.exe". */

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = verb;
    sei.lpFile = file;
    sei.lpParameters = par;
    sei.lpDirectory = (cwd && *cwd) ? cwd : NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();

        if (vapor_str_ends_with_ci(file, ".exe") && err != ERROR_CANCELLED) {
            sei.lpVerb = "runas";
            if (!ShellExecuteExA(&sei)) {
                goto done;
            }
        } else {
            goto done;
        }
    }
    if (!sei.hProcess) {
        if (wait_helpers) {
            wait_new_installer_helpers(&before, 0);
        }
        if (out_exit) {
            *out_exit = 0;
        }
        rc = 0;
        goto done;
    }
    launched = GetProcessId(sei.hProcess);
    printf("  setup is running (pid %lu); finish the wizard if a window "
           "appears\n",
           (unsigned long)launched);
    fflush(stdout);
    {
        DWORD waited = 0;

        for (;;) {
            DWORD w = WaitForSingleObject(sei.hProcess, 15000);

            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) {
                break;
            }
            waited += 15;
            printf("  still waiting for Setup (%u s)\n", waited);
            fflush(stdout);
        }
    }
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (wait_helpers) {
        wait_new_installer_helpers(&before, launched);
    }
    if (out_exit) {
        *out_exit = (int)code;
    }
    rc = 0;
done:
    if (uninit) {
        CoUninitialize();
    }
    return rc;
}

static int
find_product_dir_ex(const char *name, const char *id, char *out, size_t outsz,
                    int require_files)
{
    char pf[VAPOR_WIN_PATH];
    char pfx86[VAPOR_WIN_PATH];

    if (!out || outsz == 0) {
        return -1;
    }
    out[0] = '\0';
    if (scan_uninstall_root(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                            name, id, out, outsz, require_files)
            == 0
        || scan_uninstall_root(HKEY_LOCAL_MACHINE,
                               "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\"
                               "CurrentVersion\\Uninstall",
                               name, id, out, outsz, require_files)
               == 0
        || scan_uninstall_root(HKEY_CURRENT_USER,
                               "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                               name, id, out, outsz, require_files)
               == 0) {
        return 0;
    }
    pf[0] = pfx86[0] = '\0';
    (void)known_folder(&FOLDERID_ProgramFiles, pf, sizeof(pf));
    (void)known_folder(&FOLDERID_ProgramFilesX86, pfx86, sizeof(pfx86));
    if ((pf[0]
         && scan_program_files(pf, name, id, out, outsz, require_files) == 0)
        || (pfx86[0]
            && scan_program_files(pfx86, name, id, out, outsz, require_files)
                   == 0)) {
        return 0;
    }
    return 1;
}

int
vapor_plat_find_product_dir(const char *name, const char *id, char *out,
                            size_t outsz)
{
    return find_product_dir_ex(name, id, out, outsz, 1);
}

int
vapor_plat_guess_product_dir(const char *name, const char *id, char *out,
                             size_t outsz)
{
    return find_product_dir_ex(name, id, out, outsz, 0);
}

#endif /* _WIN32 */
