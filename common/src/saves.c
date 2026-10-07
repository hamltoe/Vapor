#include "vapor/saves.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "vapor/util.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif

static int
is_sep(char c)
{
    return c == '/' || c == '\\';
}

static void
set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        snprintf(err, errsz, "%s", msg);
    }
}

static void
set_errf(char *err, size_t errsz, const char *fmt, const char *a)
{
    if (err && errsz) {
        snprintf(err, errsz, fmt, a);
    }
}

int
vapor_install_mode_is_known(const char *mode)
{
    return mode
           && (strcmp(mode, "portable") == 0 || strcmp(mode, "setup") == 0
               || strcmp(mode, "unpack_disc") == 0
               || strcmp(mode, "keep_disc") == 0);
}

static int
is_bare_root(const char *path)
{
    size_t      n = strlen(path);
    const char *p;
    int         parts = 0;

    while (n > 1 && is_sep(path[n - 1])) {
        n--;
    }
    if (n == 0) {
        return 1;
    }
    if (n == 1 && is_sep(path[0])) {
        return 1;
    }
    if (n == 2 && isalpha((unsigned char)path[0]) && path[1] == ':') {
        return 1;
    }
    if (n == 3 && isalpha((unsigned char)path[0]) && path[1] == ':'
        && is_sep(path[2])) {
        return 1;
    }
    /* \\server or \\server\share, with no folder under the share. */
    if (n >= 2 && is_sep(path[0]) && is_sep(path[1])) {
        p = path + 2;
        while (p < path + n) {
            if (!is_sep(*p)) {
                parts++;
                while (p < path + n && !is_sep(*p)) {
                    p++;
                }
            } else {
                p++;
            }
        }
        if (parts < 3) {
            return 1;
        }
    }
    return 0;
}

int
vapor_save_path_is_valid(const char *path)
{
    const char *p;
    size_t      n;
    int         any = 0;

    if (!path || !path[0]) {
        return 0;
    }
    n = strlen(path);
    if (n >= VAPOR_SAVE_PATH_MAX) {
        return 0;
    }
    p = path;
    while (*p) {
        const char *start = p;
        size_t      seglen;

        while (*p && !is_sep(*p)) {
            if ((unsigned char)*p < 0x20) {
                return 0;
            }
            p++;
        }
        seglen = (size_t)(p - start);
        if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            return 0;
        }
        if (seglen == 1 && start[0] == '.' && p == path + n && !any) {
            return 0;
        }
        if (seglen > 0) {
            any = 1;
        }
        if (*p) {
            p++;
        }
    }
    if (!any || is_bare_root(path)) {
        return 0;
    }
    return 1;
}

static const char *
basename_of(const char *path)
{
    const char *base = path;
    const char *s;

    for (s = path; *s; s++) {
        if (is_sep(*s)) {
            base = s + 1;
        }
    }
    return base;
}

int
vapor_dos_name_is_junk(const char *path)
{
    const char *base = path ? basename_of(path) : "";

    return vapor_str_eq_ci(base, "setup.com")
           || vapor_str_eq_ci(base, "install.com");
}

int
vapor_image_is_dos_mz(const uint8_t *data, size_t len)
{
    uint32_t lfanew;

    if (!data || len < 0x40) {
        return 0;
    }
    if (data[0] != 'M' || data[1] != 'Z') {
        return 0;
    }
    lfanew = (uint32_t)data[0x3C] | ((uint32_t)data[0x3D] << 8)
             | ((uint32_t)data[0x3E] << 16) | ((uint32_t)data[0x3F] << 24);
    if (lfanew < 0x40) {
        return 1;
    }
    if ((size_t)lfanew + 4 > len) {
        return -1;
    }
    if (data[lfanew] == 'P' && data[lfanew + 1] == 'E' && data[lfanew + 2] == 0
        && data[lfanew + 3] == 0) {
        return 0;
    }
    return 1;
}

int
vapor_file_is_dos_exe(const char *path)
{
    FILE    *f;
    uint8_t  hdr[0x40];
    uint32_t lfanew;
    uint8_t  sig[4];

    if (!path || !path[0]) {
        return 0;
    }
    if (vapor_dos_name_is_junk(path)) {
        return 0;
    }
    if (vapor_str_ends_with_ci(path, ".com")) {
        return 1;
    }
    if (!vapor_str_ends_with_ci(path, ".exe")) {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return 0;
    }
    if (hdr[0] != 'M' || hdr[1] != 'Z') {
        fclose(f);
        return 0;
    }
    lfanew = (uint32_t)hdr[0x3C] | ((uint32_t)hdr[0x3D] << 8)
             | ((uint32_t)hdr[0x3E] << 16) | ((uint32_t)hdr[0x3F] << 24);
    if (lfanew < 0x40 || lfanew > 16u * 1024u * 1024u) {
        fclose(f);
        return 1;
    }
    if (fseek(f, (long)lfanew, SEEK_SET) != 0 || fread(sig, 1, 4, f) != 4) {
        fclose(f);
        return 1;
    }
    fclose(f);
    if (sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0) {
        return 0;
    }
    return 1;
}

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   n;
} grab_buf;

static size_t
grab_write(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    grab_buf *g = (grab_buf *)opaque;

    (void)file_ofs;
    if (g->n >= g->cap) {
        return 0;
    }
    if (n > g->cap - g->n) {
        n = g->cap - g->n;
    }
    memcpy(g->buf + g->n, buf, n);
    g->n += n;
    return g->n >= g->cap ? 0 : n;
}

int
vapor_zip_entry_is_dos_exe(const char *zip_path, const char *entry)
{
    mz_zip_archive zip;
    char           norm[VAPOR_SAVE_PATH_MAX];
    uint8_t        buf[65536];
    grab_buf       grab;
    int            index;
    int            kind;
    size_t         i, n;

    if (!zip_path || !entry || !entry[0]) {
        return 0;
    }
    if (vapor_dos_name_is_junk(entry)) {
        return 0;
    }
    if (vapor_str_ends_with_ci(entry, ".com")) {
        return 1;
    }
    if (!vapor_str_ends_with_ci(entry, ".exe")) {
        return 0;
    }
    n = strlen(entry);
    if (n >= sizeof(norm)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        norm[i] = entry[i] == '\\' ? '/' : entry[i];
    }
    norm[n] = '\0';

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        return 0;
    }
    index = mz_zip_reader_locate_file(&zip, norm, NULL, 0);
    if (index < 0) {
        mz_zip_reader_end(&zip);
        return 0;
    }
    grab.buf = buf;
    grab.cap = sizeof(buf);
    grab.n = 0;
    (void)mz_zip_reader_extract_to_callback(&zip, (mz_uint)index, grab_write,
                                            &grab, 0);
    mz_zip_reader_end(&zip);
    kind = vapor_image_is_dos_mz(buf, grab.n);
    if (kind < 0) {
        return 0;
    }
    return kind;
}

static int
append_span(char *out, size_t outsz, size_t *used, const char *s, size_t n)
{
    if (*used + n >= outsz) {
        return -1;
    }
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = '\0';
    return 0;
}

static int
copy_env(const char *name, char *buf, size_t bufsz)
{
    const char *v = getenv(name);

    if (!v || !v[0]) {
        return 1;
    }
    if (strlen(v) >= bufsz) {
        return -1;
    }
    snprintf(buf, bufsz, "%s", v);
    return 0;
}

/* Defined with path_stat. Maps an unset variable onto the folder that
 * variable names on XP, Vista, or Linux when that folder exists. */
static int env_fallback(const char *name, char *buf, size_t bufsz);

static int
env_token(const char *name, char *buf, size_t bufsz)
{
    int rc = copy_env(name, buf, bufsz);

    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    if (vapor_str_eq_ci(name, "HOME")) {
        rc = copy_env("USERPROFILE", buf, bufsz);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
    }
    return env_fallback(name, buf, bufsz);
}

static void
native_seps(char *s)
{
    for (; *s; s++) {
#if defined(_WIN32)
        if (*s == '/') {
            *s = '\\';
        }
#else
        if (*s == '\\') {
            *s = '/';
        }
#endif
    }
}

static int
is_absolute_path(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    if (is_sep(path[0])) {
        return 1;
    }
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        return 1;
    }
    return 0;
}

int
vapor_save_resolve(const char *spec, const char *install_dir, char *out,
                   size_t outsz, char *err, size_t errsz)
{
    char        expanded[VAPOR_SAVE_PATH_MAX];
    size_t      used = 0;
    const char *p;

    if (!vapor_save_path_is_valid(spec)) {
        set_errf(err, errsz, "save path is not allowed: %s",
                 spec ? spec : "");
        return -1;
    }
    if (!install_dir || !install_dir[0] || !out || outsz == 0) {
        set_err(err, errsz, "save path is missing an install directory");
        return -1;
    }
    expanded[0] = '\0';
    p = spec;
    while (*p) {
        if (strncmp(p, "$INSTALL_DIR", 12) == 0
            && (p[12] == '\0' || is_sep(p[12]))) {
            if (append_span(expanded, sizeof(expanded), &used, install_dir,
                            strlen(install_dir))
                != 0) {
                set_err(err, errsz, "save path is too long");
                return -1;
            }
            p += 12;
            continue;
        }
        if (strncmp(p, "$HOME", 5) == 0 && (p[5] == '\0' || is_sep(p[5]))) {
            char   home[VAPOR_SAVE_PATH_MAX];
            int    rc = env_token("HOME", home, sizeof(home));

            if (rc != 0) {
                set_err(err, errsz, "save path needs HOME or USERPROFILE");
                return rc < 0 ? -1 : 1;
            }
            if (append_span(expanded, sizeof(expanded), &used, home, strlen(home))
                != 0) {
                set_err(err, errsz, "save path is too long");
                return -1;
            }
            p += 5;
            continue;
        }
        if (*p == '%') {
            const char *end = strchr(p + 1, '%');
            char        name[128];
            char        val[VAPOR_SAVE_PATH_MAX];
            size_t      namelen;
            int         rc;

            if (!end || end == p + 1) {
                set_errf(err, errsz, "save path has an empty environment token: %s",
                         spec);
                return -1;
            }
            namelen = (size_t)(end - (p + 1));
            if (namelen >= sizeof(name)) {
                set_err(err, errsz, "save path environment name is too long");
                return -1;
            }
            memcpy(name, p + 1, namelen);
            name[namelen] = '\0';
            rc = env_token(name, val, sizeof(val));
            if (rc != 0) {
                set_errf(err, errsz, "save path environment is unset: %s", name);
                return rc < 0 ? -1 : 1;
            }
            if (append_span(expanded, sizeof(expanded), &used, val, strlen(val))
                != 0) {
                set_err(err, errsz, "save path is too long");
                return -1;
            }
            p = end + 1;
            continue;
        }
        if (append_span(expanded, sizeof(expanded), &used, p, 1) != 0) {
            set_err(err, errsz, "save path is too long");
            return -1;
        }
        p++;
    }

    if (!is_absolute_path(expanded)) {
        int n = snprintf(out, outsz, "%s/%s", install_dir, expanded);
        if (n < 0 || (size_t)n >= outsz) {
            set_err(err, errsz, "save path is too long");
            return -1;
        }
    } else if (strlen(expanded) >= outsz) {
        set_err(err, errsz, "save path is too long");
        return -1;
    } else {
        memcpy(out, expanded, used + 1);
    }
    native_seps(out);
    if (!vapor_save_path_is_valid(out)) {
        set_errf(err, errsz, "save path is not allowed: %s", out);
        return -1;
    }
    return 0;
}

static int
path_stat(const char *path, int *is_dir, uint64_t *size)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
        return 0;
    }
    if (is_dir) {
        *is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (size) {
        *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    }
    return 1;
#else
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    if (is_dir) {
        *is_dir = S_ISDIR(st.st_mode);
    }
    if (size) {
        *size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    }
    return 1;
#endif
}

static int
mkdir_one(const char *path)
{
#if defined(_WIN32)
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return 0;
    }
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
#endif
    return path_stat(path, NULL, NULL) ? 0 : -1;
}

static int
profile_dir(char *buf, size_t bufsz)
{
    const char *drive;
    const char *path;
    int         rc = copy_env("USERPROFILE", buf, bufsz);

    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    rc = copy_env("HOME", buf, bufsz);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    drive = getenv("HOMEDRIVE");
    path = getenv("HOMEPATH");
    if (!drive || !drive[0] || !path || !path[0]) {
        return 1;
    }
    if ((size_t)snprintf(buf, bufsz, "%s%s", drive, path) >= bufsz) {
        return -1;
    }
    return 0;
}

static int
join_existing_dir(char *buf, size_t bufsz, const char *profile, const char *rel)
{
    int is_dir = 0;

    if ((size_t)snprintf(buf, bufsz, "%s/%s", profile, rel) >= bufsz) {
        return -1;
    }
    native_seps(buf);
    if (path_stat(buf, &is_dir, NULL) && is_dir) {
        return 0;
    }
    return 1;
}

static int
env_fallback(const char *name, char *buf, size_t bufsz)
{
    char profile[VAPOR_SAVE_PATH_MAX];
    int  rc;

    if (vapor_str_eq_ci(name, "HOME") || vapor_str_eq_ci(name, "USERPROFILE")) {
        return profile_dir(buf, bufsz);
    }
    rc = profile_dir(profile, sizeof(profile));
    if (rc != 0) {
        return rc;
    }
    /* XP uses "Local Settings" / "Application Data". Vista and later use
     * AppData. A Linux client resolving a Windows spec uses the XDG folder
     * when that directory is already there. */
    if (vapor_str_eq_ci(name, "LOCALAPPDATA")) {
        if (join_existing_dir(buf, bufsz, profile,
                              "Local Settings/Application Data")
            == 0) {
            return 0;
        }
        if (join_existing_dir(buf, bufsz, profile, "AppData/Local") == 0) {
            return 0;
        }
        if (join_existing_dir(buf, bufsz, profile, ".local/share") == 0) {
            return 0;
        }
        return 1;
    }
    if (vapor_str_eq_ci(name, "APPDATA")) {
        if (join_existing_dir(buf, bufsz, profile, "Application Data") == 0) {
            return 0;
        }
        if (join_existing_dir(buf, bufsz, profile, "AppData/Roaming") == 0) {
            return 0;
        }
        if (join_existing_dir(buf, bufsz, profile, ".config") == 0) {
            return 0;
        }
        return 1;
    }
    return 1;
}

static int
mkdir_p(const char *path)
{
    char   buf[VAPOR_SAVE_PATH_MAX];
    size_t n, i, start;

    if (!path || !path[0]) {
        return -1;
    }
    n = strlen(path);
    if (n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, n + 1);
    start = 0;
    if (is_sep(buf[0]) && n > 1 && is_sep(buf[1])) {
        /* Skip \\server\share before creating folders under it. */
        int parts = 0;
        i = 2;
        while (i < n && parts < 2) {
            if (!is_sep(buf[i])) {
                parts++;
                while (i < n && !is_sep(buf[i])) {
                    i++;
                }
            } else {
                i++;
            }
        }
        start = i;
    } else if (isalpha((unsigned char)buf[0]) && buf[1] == ':') {
        start = (n > 2 && is_sep(buf[2])) ? 3 : 2;
    } else if (is_sep(buf[0])) {
        start = 1;
    }
    for (i = start; i < n; i++) {
        if (!is_sep(buf[i])) {
            continue;
        }
        buf[i] = '\0';
        if (buf[0] && mkdir_one(buf) != 0) {
            return -1;
        }
        buf[i] = '/';
    }
    return mkdir_one(buf);
}

typedef struct {
    char    *archive_name;
    char    *abs;
    uint64_t size;
} save_file;

static int
cmp_save_file(const void *a, const void *b)
{
    const save_file *fa = (const save_file *)a;
    const save_file *fb = (const save_file *)b;
    return strcmp(fa->archive_name, fb->archive_name);
}

static void
free_save_files(save_file *files, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(files[i].archive_name);
        free(files[i].abs);
    }
    free(files);
}

static int
push_file(save_file **files, size_t *n, size_t *cap, uint64_t *bytes,
          const char *archive_name, const char *abs, uint64_t size, char *err,
          size_t errsz)
{
    save_file *grown;
    char      *name;
    char      *path;

    if (*n >= VAPOR_SAVE_SYNC_MAX_FILES) {
        set_err(err, errsz, "save sync has more than 10000 files");
        return -1;
    }
    if (*bytes > VAPOR_SAVE_SYNC_MAX_BYTES
        || size > VAPOR_SAVE_SYNC_MAX_BYTES - *bytes) {
        set_err(err, errsz, "save sync is larger than 256 MiB");
        return -1;
    }
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 32;
        grown = (save_file *)realloc(*files, next * sizeof(*grown));
        if (!grown) {
            set_err(err, errsz, "out of memory");
            return -1;
        }
        *files = grown;
        *cap = next;
    }
    name = vapor_strdup(archive_name);
    path = vapor_strdup(abs);
    if (!name || !path) {
        free(name);
        free(path);
        set_err(err, errsz, "out of memory");
        return -1;
    }
    (*files)[*n].archive_name = name;
    (*files)[*n].abs = path;
    (*files)[*n].size = size;
    (*n)++;
    *bytes += size;
    return 0;
}

#if defined(_WIN32)
static void
slashify(char *s)
{
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static int
collect_tree(const char *root, const char *rel, const char *prefix, save_file **files,
             size_t *n, size_t *cap, uint64_t *bytes, char *err, size_t errsz)
{
    char             pattern[VAPOR_SAVE_PATH_MAX];
    char             child_abs[VAPOR_SAVE_PATH_MAX];
    char             child_rel[VAPOR_SAVE_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              rc = 0;

    if (rel[0]) {
        if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\%s\\*", root, rel)
            >= sizeof(pattern)) {
            set_err(err, errsz, "save path is too long");
            return -1;
        }
    } else if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", root)
               >= sizeof(pattern)) {
        set_err(err, errsz, "save path is too long");
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        uint64_t sz;
        int      nprint;

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (strcmp(fd.cFileName, ".vapor") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (rel[0]) {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                              fd.cFileName);
        } else {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s", fd.cFileName);
        }
        if (nprint < 0 || (size_t)nprint >= sizeof(child_rel)
            || (size_t)snprintf(child_abs, sizeof(child_abs), "%s\\%s", root,
                                child_rel)
                   >= sizeof(child_abs)) {
            set_err(err, errsz, "save path is too long");
            rc = -1;
            break;
        }
        slashify(child_rel);
        native_seps(child_abs);
        if (strstr(child_rel, "..")) {
            set_err(err, errsz, "save path is not allowed");
            rc = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (collect_tree(root, child_rel, prefix, files, n, cap, bytes, err,
                             errsz)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        {
            char archive[VAPOR_SAVE_PATH_MAX];
            if ((size_t)snprintf(archive, sizeof(archive), "%s/%s", prefix,
                                 child_rel)
                >= sizeof(archive)) {
                set_err(err, errsz, "save path is too long");
                rc = -1;
                break;
            }
            if (push_file(files, n, cap, bytes, archive, child_abs, sz, err,
                          errsz)
                != 0) {
                rc = -1;
                break;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
}
#else
static int
collect_tree(const char *root, const char *rel, const char *prefix, save_file **files,
             size_t *n, size_t *cap, uint64_t *bytes, char *err, size_t errsz)
{
    char           abs[VAPOR_SAVE_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", root, rel) >= sizeof(abs)) {
            set_err(err, errsz, "save path is too long");
            return -1;
        }
    } else if (strlen(root) >= sizeof(abs)) {
        set_err(err, errsz, "save path is too long");
        return -1;
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }
    d = opendir(abs);
    if (!d) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPOR_SAVE_PATH_MAX];
        char        child_abs[VAPOR_SAVE_PATH_MAX];
        struct stat st;
        int         nprint;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(ent->d_name, ".vapor") == 0) {
            continue;
        }
        if (rel[0]) {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                              ent->d_name);
        } else {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (nprint < 0 || (size_t)nprint >= sizeof(child_rel)
            || (size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", root,
                                child_rel)
                   >= sizeof(child_abs)) {
            set_err(err, errsz, "save path is too long");
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (collect_tree(root, child_rel, prefix, files, n, cap, bytes, err,
                             errsz)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        {
            char archive[VAPOR_SAVE_PATH_MAX];
            if ((size_t)snprintf(archive, sizeof(archive), "%s/%s", prefix,
                                 child_rel)
                >= sizeof(archive)) {
                set_err(err, errsz, "save path is too long");
                rc = -1;
                break;
            }
            if (push_file(files, n, cap, bytes, archive, child_abs,
                          (uint64_t)st.st_size, err, errsz)
                != 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(d);
    return rc;
}
#endif

static void
note_missing(char *err, size_t errsz, const char *spec)
{
    char   line[VAPOR_SAVE_PATH_MAX + 32];
    size_t have;

    snprintf(line, sizeof(line), "missing save path: %s", spec ? spec : "");
    if (!err || errsz == 0) {
        return;
    }
    have = strlen(err);
    if (have == 0) {
        snprintf(err, errsz, "%s", line);
        return;
    }
    if (have + 2 < errsz) {
        snprintf(err + have, errsz - have, "\n%s", line);
    }
}

#define VAPOR_SAVE_MARKER "_vapor_present.txt"

static int
paths_under(const char *parent, const char *child)
{
    size_t n = strlen(parent);

    if (n == 0 || strlen(child) < n) {
        return 0;
    }
#if defined(_WIN32)
    if (_strnicmp(child, parent, n) != 0) {
        return 0;
    }
#else
    if (strncmp(child, parent, n) != 0) {
        return 0;
    }
#endif
    return child[n] == '\0' || is_sep(child[n]);
}

static int
is_save_dirname(const char *name)
{
    return vapor_str_eq_ci(name, "save") || vapor_str_eq_ci(name, "saves")
           || vapor_str_eq_ci(name, "savegames");
}

static int
push_rel(char ***out, size_t *n, const char *rel)
{
    char **grown;
    char  *copy;

    if (*n >= VAPOR_SAVE_MAX_PATHS) {
        return 0;
    }
    grown = (char **)realloc(*out, (*n + 1) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    *out = grown;
    copy = vapor_strdup(rel);
    if (!copy) {
        return -1;
    }
    (*out)[*n] = copy;
    (*n)++;
    return 0;
}

static int
cmp_strptr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

#if defined(_WIN32)
static int
discover_walk(const char *install, const char *rel, int depth, char ***out,
              size_t *n)
{
    char             pattern[VAPOR_SAVE_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              rc = 0;

    if (depth > 6) {
        return 0;
    }
    if (rel[0]) {
        if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\%s\\*", install, rel)
            >= sizeof(pattern)) {
            return -1;
        }
    } else if ((size_t)snprintf(pattern, sizeof(pattern), "%s\\*", install)
               >= sizeof(pattern)) {
        return -1;
    }
    native_seps(pattern);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        char child_rel[VAPOR_SAVE_PATH_MAX];
        int  nprint;

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0
            || strcmp(fd.cFileName, ".vapor") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (rel[0]) {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                              fd.cFileName);
        } else {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s", fd.cFileName);
        }
        if (nprint < 0 || (size_t)nprint >= sizeof(child_rel)) {
            rc = -1;
            break;
        }
        slashify(child_rel);
        if (is_save_dirname(fd.cFileName)) {
            if (push_rel(out, n, child_rel) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (discover_walk(install, child_rel, depth + 1, out, n) != 0) {
            rc = -1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
}
#else
static int
discover_walk(const char *install, const char *rel, int depth, char ***out,
              size_t *n)
{
    char           abs[VAPOR_SAVE_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (depth > 6) {
        return 0;
    }
    if (rel[0]) {
        if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", install, rel) >= sizeof(abs)) {
            return -1;
        }
    } else if (strlen(install) >= sizeof(abs)) {
        return -1;
    } else {
        snprintf(abs, sizeof(abs), "%s", install);
    }
    d = opendir(abs);
    if (!d) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPOR_SAVE_PATH_MAX];
        char        child_abs[VAPOR_SAVE_PATH_MAX];
        struct stat st;
        int         nprint;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0
            || strcmp(ent->d_name, ".vapor") == 0) {
            continue;
        }
        if (rel[0]) {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                              ent->d_name);
        } else {
            nprint = snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (nprint < 0 || (size_t)nprint >= sizeof(child_rel)
            || (size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", install,
                                child_rel)
                   >= sizeof(child_abs)) {
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (is_save_dirname(ent->d_name)) {
            if (push_rel(out, n, child_rel) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (discover_walk(install, child_rel, depth + 1, out, n) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}
#endif

static void
free_rels(char **rels, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(rels[i]);
    }
    free(rels);
}

static int
marker_add(char **text, size_t *len, size_t *cap, const char *line)
{
    size_t need = strlen(line) + 1;
    char  *grown;

    if (*len + need + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : 256;
        while (next < *len + need + 1) {
            next *= 2;
        }
        grown = (char *)realloc(*text, next);
        if (!grown) {
            return -1;
        }
        *text = grown;
        *cap = next;
    }
    memcpy(*text + *len, line, need - 1);
    (*text)[*len + (need - 1)] = '\n';
    *len += need;
    (*text)[*len] = '\0';
    return 0;
}

int
vapor_saves_pack(const char *const *specs, size_t nspecs, const char *install_dir,
                 const char *zip_path, int *out_missing, char *err, size_t errsz)
{
    save_file *files = NULL;
    char     **found = NULL;
    char      *marker = NULL;
    char       kept[VAPOR_SAVE_MAX_PATHS][VAPOR_SAVE_PATH_MAX];
    size_t     n = 0, cap = 0, nfound = 0, nkept = 0, i;
    size_t     marker_len = 0, marker_cap = 0;
    uint64_t   bytes = 0;
    int        missing = 0;
    int        present = 0;
    mz_zip_archive zip;

    if (out_missing) {
        *out_missing = 0;
    }
    if (err && errsz) {
        err[0] = '\0';
    }
    if (!zip_path || !zip_path[0] || !install_dir || !install_dir[0]
        || (nspecs > 0 && !specs)) {
        set_err(err, errsz, "save archive path is missing");
        return -1;
    }
    for (i = 0; i < nspecs; i++) {
        char     resolved[VAPOR_SAVE_PATH_MAX];
        char     prefix[32];
        int      is_dir = 0;
        uint64_t sz = 0;
        int      resolved_rc;

        resolved_rc = vapor_save_resolve(specs[i], install_dir, resolved,
                                         sizeof(resolved), err, errsz);
        if (resolved_rc < 0) {
            free_save_files(files, n);
            free(marker);
            return -1;
        }
        if (resolved_rc > 0 || !path_stat(resolved, &is_dir, &sz)) {
            missing = 1;
            note_missing(err, errsz, specs[i]);
            continue;
        }
        snprintf(prefix, sizeof(prefix), "%u", (unsigned)i);
        if (!is_dir) {
            if (push_file(&files, &n, &cap, &bytes, prefix, resolved, sz, err,
                          errsz)
                != 0) {
                free_save_files(files, n);
                free(marker);
                return -1;
            }
        } else if (collect_tree(resolved, "", prefix, &files, &n, &cap, &bytes,
                                err, errsz)
                   != 0) {
            free_save_files(files, n);
            free(marker);
            return -1;
        }
        if (nkept < VAPOR_SAVE_MAX_PATHS) {
            snprintf(kept[nkept], sizeof(kept[nkept]), "%s", resolved);
            nkept++;
        }
        if (marker_add(&marker, &marker_len, &marker_cap, prefix) != 0) {
            free_save_files(files, n);
            free(marker);
            set_err(err, errsz, "out of memory");
            return -1;
        }
        present++;
    }
    if (discover_walk(install_dir, "", 0, &found, &nfound) != 0) {
        free_save_files(files, n);
        free_rels(found, nfound);
        free(marker);
        set_err(err, errsz, "cannot look for save directories");
        return -1;
    }
    if (nfound > 1) {
        qsort(found, nfound, sizeof(*found), cmp_strptr);
    }
    for (i = 0; i < nfound; i++) {
        char   resolved[VAPOR_SAVE_PATH_MAX];
        char   prefix[VAPOR_SAVE_PATH_MAX];
        size_t k;
        int    covered = 0;

        if ((size_t)snprintf(resolved, sizeof(resolved), "%s/%s", install_dir,
                             found[i])
            >= sizeof(resolved)) {
            continue;
        }
        native_seps(resolved);
        for (k = 0; k < nkept; k++) {
            if (paths_under(kept[k], resolved)) {
                covered = 1;
                break;
            }
        }
        if (covered) {
            continue;
        }
        if ((size_t)snprintf(prefix, sizeof(prefix), "r/%s", found[i])
            >= sizeof(prefix)) {
            continue;
        }
        if (collect_tree(resolved, "", prefix, &files, &n, &cap, &bytes, err,
                         errsz)
            != 0) {
            free_save_files(files, n);
            free_rels(found, nfound);
            free(marker);
            return -1;
        }
        if (marker_add(&marker, &marker_len, &marker_cap, prefix) != 0) {
            free_save_files(files, n);
            free_rels(found, nfound);
            free(marker);
            set_err(err, errsz, "out of memory");
            return -1;
        }
        present++;
    }
    free_rels(found, nfound);
    if (present == 0) {
        if (out_missing) {
            *out_missing = 1;
        }
        free_save_files(files, n);
        free(marker);
        remove(zip_path);
        return 0;
    }
    if (n > 1) {
        qsort(files, n, sizeof(*files), cmp_save_file);
    }
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, zip_path, 0)) {
        set_errf(err, errsz, "cannot create save archive %s", zip_path);
        free_save_files(files, n);
        free(marker);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!mz_zip_writer_add_file(&zip, files[i].archive_name, files[i].abs,
                                    NULL, 0, MZ_NO_COMPRESSION)) {
            set_errf(err, errsz, "cannot archive %s", files[i].abs);
            mz_zip_writer_end(&zip);
            remove(zip_path);
            free_save_files(files, n);
            free(marker);
            return -1;
        }
    }
    if (marker
        && !mz_zip_writer_add_mem(&zip, VAPOR_SAVE_MARKER, marker, marker_len,
                                  MZ_NO_COMPRESSION)) {
        set_err(err, errsz, "cannot archive the save manifest");
        mz_zip_writer_end(&zip);
        remove(zip_path);
        free_save_files(files, n);
        free(marker);
        return -1;
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        set_err(err, errsz, "cannot finish the save archive");
        mz_zip_writer_end(&zip);
        remove(zip_path);
        free_save_files(files, n);
        free(marker);
        return -1;
    }
    mz_zip_writer_end(&zip);
    free_save_files(files, n);
    free(marker);
    if (out_missing) {
        *out_missing = missing;
    }
    return 0;
}

static int
slot_covers(const char *slot, size_t nslot, const char *entry)
{
    size_t n = strlen(entry);

    if (nslot == 0) {
        return 0;
    }
    if (n == nslot && memcmp(entry, slot, nslot) == 0) {
        return 1;
    }
    return n > nslot && memcmp(entry, slot, nslot) == 0 && entry[nslot] == '/';
}

static int
marker_covers(const char *marker, size_t marker_len, const char *entry)
{
    const char *p;
    const char *end;

    if (!marker || marker_len == 0) {
        return 0;
    }
    p = marker;
    end = marker + marker_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t      n = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (n > 0 && slot_covers(p, n, entry)) {
            return 1;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}

typedef struct {
    char  *buf;
    size_t n;
    size_t cap;
} mem_grab;

static size_t
mem_grab_write(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    mem_grab *g = (mem_grab *)opaque;
    char     *grown;

    (void)file_ofs;
    if (g->n + n + 1 > g->cap) {
        size_t next = g->cap ? g->cap * 2 : 256;
        while (next < g->n + n + 1) {
            next *= 2;
        }
        grown = (char *)realloc(g->buf, next);
        if (!grown) {
            return 0;
        }
        g->buf = grown;
        g->cap = next;
    }
    memcpy(g->buf + g->n, buf, n);
    g->n += n;
    g->buf[g->n] = '\0';
    return n;
}

static int
read_zip_marker(mz_zip_archive *zip, char **out, size_t *outn)
{
    mz_uint  i, count;
    mem_grab grab;

    *out = NULL;
    *outn = 0;
    count = mz_zip_reader_get_num_files(zip);
    for (i = 0; i < count; i++) {
        char name[128];

        mz_zip_reader_get_filename(zip, i, name, sizeof(name));
        if (strcmp(name, VAPOR_SAVE_MARKER) != 0) {
            continue;
        }
        memset(&grab, 0, sizeof(grab));
        if (!mz_zip_reader_extract_to_callback(zip, i, mem_grab_write, &grab, 0)
            || !grab.buf) {
            free(grab.buf);
            return -1;
        }
        *out = grab.buf;
        *outn = grab.n;
        return 0;
    }
    return 0;
}

static int
copy_zip_entries(mz_zip_archive *writer, mz_zip_archive *reader,
                 const char *marker, size_t marker_len, int skip_covered)
{
    mz_uint i, count = mz_zip_reader_get_num_files(reader);

    for (i = 0; i < count; i++) {
        char name[VAPOR_SAVE_PATH_MAX];

        if (mz_zip_reader_is_file_a_directory(reader, i)) {
            continue;
        }
        mz_zip_reader_get_filename(reader, i, name, sizeof(name));
        if (strcmp(name, VAPOR_SAVE_MARKER) == 0) {
            continue;
        }
        if (skip_covered && marker_covers(marker, marker_len, name)) {
            continue;
        }
        if (!mz_zip_writer_add_from_zip_reader(writer, reader, i)) {
            return -1;
        }
    }
    return 0;
}

int
vapor_saves_merge(const char *fresh_zip, const char *account_zip,
                  const char *out_zip, char *err, size_t errsz)
{
    mz_zip_archive fresh;
    mz_zip_archive account;
    mz_zip_archive out;
    char          *marker = NULL;
    size_t         marker_len = 0;

    if (err && errsz) {
        err[0] = '\0';
    }
    memset(&fresh, 0, sizeof(fresh));
    memset(&account, 0, sizeof(account));
    memset(&out, 0, sizeof(out));
    if (!mz_zip_reader_init_file(&fresh, fresh_zip, 0)
        || !mz_zip_reader_init_file(&account, account_zip, 0)) {
        mz_zip_reader_end(&fresh);
        mz_zip_reader_end(&account);
        set_err(err, errsz, "cannot read save archive");
        return -1;
    }
    if (read_zip_marker(&fresh, &marker, &marker_len) != 0) {
        mz_zip_reader_end(&fresh);
        mz_zip_reader_end(&account);
        set_err(err, errsz, "cannot read the save manifest");
        return -1;
    }
    if (!mz_zip_writer_init_file(&out, out_zip, 0)) {
        free(marker);
        mz_zip_reader_end(&fresh);
        mz_zip_reader_end(&account);
        set_errf(err, errsz, "cannot create save archive %s", out_zip);
        return -1;
    }
    if (copy_zip_entries(&out, &account, marker, marker_len, 1) != 0
        || copy_zip_entries(&out, &fresh, marker, marker_len, 0) != 0
        || (marker
            && !mz_zip_writer_add_mem(&out, VAPOR_SAVE_MARKER, marker, marker_len,
                                      MZ_NO_COMPRESSION))
        || !mz_zip_writer_finalize_archive(&out)) {
        set_err(err, errsz, "cannot merge save archives");
        mz_zip_writer_end(&out);
        mz_zip_reader_end(&fresh);
        mz_zip_reader_end(&account);
        free(marker);
        remove(out_zip);
        return -1;
    }
    mz_zip_writer_end(&out);
    mz_zip_reader_end(&fresh);
    mz_zip_reader_end(&account);
    free(marker);
    return 0;
}

static int
entry_under_root(const char *root, const char *dest)
{
    size_t n = strlen(root);

    if (n == 0 || strlen(dest) < n) {
        return 0;
    }
#if defined(_WIN32)
    if (_strnicmp(dest, root, n) != 0) {
        return 0;
    }
#else
    if (strncmp(dest, root, n) != 0) {
        return 0;
    }
#endif
    return dest[n] == '\0' || is_sep(dest[n]);
}

static int
parse_entry(const char *name, unsigned *index, const char **rel)
{
    char       *end = NULL;
    unsigned long v;

    if (!name || !name[0]) {
        return -1;
    }
    v = strtoul(name, &end, 10);
    if (end == name) {
        return -1;
    }
    *index = (unsigned)v;
    if (*end == '\0') {
        *rel = NULL;
        return 0;
    }
    if (*end != '/') {
        return -1;
    }
    *rel = end + 1;
    if ((*rel)[0] == '\0' || strstr(*rel, "/../") || strncmp(*rel, "../", 3) == 0
        || strcmp(*rel, "..") == 0) {
        return -1;
    }
    return 0;
}

int
vapor_saves_unpack(const char *zip_path, const char *const *specs, size_t nspecs,
                   const char *install_dir, int *out_skipped, char *err,
                   size_t errsz)
{
    mz_zip_archive zip;
    mz_uint        i, count;
    char          *roots = NULL;
    int           *ok = NULL;
    uint64_t       bytes = 0;
    size_t         files = 0;
    int            skipped = 0;

    if (out_skipped) {
        *out_skipped = 0;
    }
    if (err && errsz) {
        err[0] = '\0';
    }
    if (!zip_path || (nspecs > 0 && !specs) || !install_dir || !install_dir[0]) {
        set_err(err, errsz, "save archive path is missing");
        return -1;
    }
    roots = (char *)calloc(nspecs, VAPOR_SAVE_PATH_MAX);
    ok = (int *)calloc(nspecs ? nspecs : 1, sizeof(*ok));
    if (!roots || !ok) {
        free(roots);
        free(ok);
        set_err(err, errsz, "out of memory");
        return -1;
    }
    for (i = 0; i < nspecs; i++) {
        char *slot = roots + (size_t)i * VAPOR_SAVE_PATH_MAX;
        int   rc = vapor_save_resolve(specs[i], install_dir, slot,
                                      VAPOR_SAVE_PATH_MAX, err, errsz);

        if (rc < 0) {
            free(roots);
            free(ok);
            return -1;
        }
        if (rc > 0) {
            /* This OS has no such folder. Leave that slot in the archive. */
            continue;
        }
        ok[i] = 1;
    }

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        free(roots);
        free(ok);
        set_errf(err, errsz, "cannot read save archive %s", zip_path);
        return -1;
    }
    count = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < count; i++) {
        char            name[VAPOR_SAVE_PATH_MAX];
        char            dest[VAPOR_SAVE_PATH_MAX];
        const char     *rel = NULL;
        unsigned        index = 0;
        mz_zip_archive_file_stat st;
        char           *root;
        char           *slash;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (strcmp(name, "_vapor_present.txt") == 0) {
            continue;
        }
        if (strncmp(name, "r/", 2) == 0) {
            const char *relpath = name + 2;
            char        install_native[VAPOR_SAVE_PATH_MAX];

            if (!relpath[0] || strstr(relpath, "..") || is_absolute_path(relpath)) {
                continue;
            }
            if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", install_dir, relpath)
                >= sizeof(dest)) {
                mz_zip_reader_end(&zip);
                free(roots);
                free(ok);
                set_err(err, errsz, "save path is too long");
                return -1;
            }
            native_seps(dest);
            snprintf(install_native, sizeof(install_native), "%s", install_dir);
            native_seps(install_native);
            if (!entry_under_root(install_native, dest)) {
                continue;
            }
            slash = dest + strlen(dest);
            while (slash > dest && !is_sep(slash[-1])) {
                slash--;
            }
            if (slash > dest) {
                char saved = *slash;
                *slash = '\0';
                if (mkdir_p(dest) != 0) {
                    *slash = saved;
                    skipped++;
                    note_missing(err, errsz, relpath);
                    continue;
                }
                *slash = saved;
            }
            if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
                mz_zip_reader_end(&zip);
                free(roots);
                free(ok);
                set_errf(err, errsz, "cannot restore %s", dest);
                return -1;
            }
            continue;
        }
        if (parse_entry(name, &index, &rel) != 0 || index >= nspecs || !ok[index]) {
            continue;
        }
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            free(roots);
            free(ok);
            set_err(err, errsz, "save archive entry is unreadable");
            return -1;
        }
        if (files >= VAPOR_SAVE_SYNC_MAX_FILES
            || bytes > VAPOR_SAVE_SYNC_MAX_BYTES
            || st.m_uncomp_size > VAPOR_SAVE_SYNC_MAX_BYTES - bytes) {
            mz_zip_reader_end(&zip);
            free(roots);
            free(ok);
            set_err(err, errsz, "save archive exceeds the sync limit");
            return -1;
        }
        root = roots + (size_t)index * VAPOR_SAVE_PATH_MAX;
        if (!rel) {
            if (strlen(root) >= sizeof(dest)) {
                mz_zip_reader_end(&zip);
                free(roots);
                free(ok);
                set_err(err, errsz, "save path is too long");
                return -1;
            }
            snprintf(dest, sizeof(dest), "%s", root);
        } else if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", root, rel)
                   >= sizeof(dest)) {
            mz_zip_reader_end(&zip);
            free(roots);
            free(ok);
            set_err(err, errsz, "save path is too long");
            return -1;
        }
        native_seps(dest);
        if (!entry_under_root(root, dest)) {
            continue;
        }
        slash = dest + strlen(dest);
        while (slash > dest && !is_sep(slash[-1])) {
            slash--;
        }
        if (slash > dest) {
            char saved = *slash;
            *slash = '\0';
            if (mkdir_p(dest) != 0) {
                *slash = saved;
                ok[index] = 0;
                skipped++;
                note_missing(err, errsz, specs[index]);
                continue;
            }
            *slash = saved;
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
            mz_zip_reader_end(&zip);
            free(roots);
            free(ok);
            set_errf(err, errsz, "cannot restore %s", dest);
            return -1;
        }
        files++;
        bytes += st.m_uncomp_size;
    }
    mz_zip_reader_end(&zip);
    free(roots);
    free(ok);
    if (out_skipped) {
        *out_skipped = skipped;
    }
    return 0;
}
