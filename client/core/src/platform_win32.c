#if defined(_WIN32)

#include "platform.h"

#include <windows.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/buf.h"

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
    }

    vapor_buf_free(&cmd);
    return rc;
}

#endif /* _WIN32 */
