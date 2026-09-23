/* Classify Windows installer families and build unattended command lines so
 * one Install click can finish a kit without per-title special cases. */

#include "installer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "vapor/util.h"

static const char *
basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');

    if (b > s) {
        s = b;
    }
    return s ? s + 1 : path;
}

static int
join_dir(char *out, size_t outsz, const char *dir, const char *name)
{
    int n = snprintf(out, outsz, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
sibling_exists(const char *dir, const char *name)
{
    char path[VAPOR_WIN_PATH];

    if (join_dir(path, sizeof(path), dir, name) != 0) {
        return 0;
    }
    vapor_plat_native_path(path);
    return vapor_plat_exists(path);
}

typedef struct {
    int  found;
    char rel[VAPOR_WIN_PATH];
} msi_hit;

static int
msi_in_dir_cb(const char *rel, const char *abs, void *ud)
{
    msi_hit    *h = ud;
    const char *slash;

    (void)abs;
    slash = strchr(rel, '/');
    if (slash) {
        return 0;
    }
    if (!vapor_str_ends_with_ci(rel, ".msi")) {
        return 0;
    }
    if (vapor_str_has_prefix(rel, "ISScript") || vapor_str_has_prefix(rel, "isscript")) {
        return 0;
    }
    snprintf(h->rel, sizeof(h->rel), "%s", rel);
    h->found = 1;
    return 1;
}

static int
dir_has_product_msi(const char *dir)
{
    msi_hit h;

    memset(&h, 0, sizeof(h));
    vapor_plat_walk_files(dir, msi_in_dir_cb, &h);
    return h.found;
}

static int
scan_chunk(const char *buf, size_t n, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || n < nlen) {
        return 0;
    }
    for (i = 0; i + nlen <= n; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Read a bounded prefix (and, for larger files, a suffix) so a 4 GiB GOG
 * bootstrap does not get slurped whole. */
static int
file_has_ascii(const char *path, const char *needle)
{
    FILE          *f;
    char           buf[64 * 1024];
    size_t         n, nlen, keep = 0;
    unsigned long  limit = 2u * 1024u * 1024u;
    unsigned long  seen = 0;
    long           sz;

    nlen = strlen(needle);
    if (nlen == 0 || nlen > 128) {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    while (seen < limit && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        size_t total = keep + n;
        if (scan_chunk(buf, total, needle)) {
            fclose(f);
            return 1;
        }
        keep = nlen - 1;
        if (keep > total) {
            keep = total;
        }
        memmove(buf, buf + total - keep, keep);
        seen += (unsigned long)n;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
        if (sz > (long)limit + 1024 * 1024) {
            keep = 0;
            if (fseek(f, sz - 1024 * 1024, SEEK_SET) == 0) {
                while ((n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
                    size_t total = keep + n;
                    if (scan_chunk(buf, total, needle)) {
                        fclose(f);
                        return 1;
                    }
                    keep = nlen - 1;
                    if (keep > total) {
                        keep = total;
                    }
                    memmove(buf, buf + total - keep, keep);
                }
            }
        }
    }
    fclose(f);
    return 0;
}

const char *
vapor_inst_kind_name(vapor_inst_kind kind)
{
    switch (kind) {
    case VAPOR_INST_INNO:
        return "inno";
    case VAPOR_INST_NSIS:
        return "nsis";
    case VAPOR_INST_MSI:
        return "msi";
    case VAPOR_INST_ISHIELD:
        return "installshield";
    case VAPOR_INST_EXE:
        return "exe";
    case VAPOR_INST_ISO:
        return "iso";
    default:
        return "none";
    }
}

vapor_inst_kind
vapor_inst_detect(const char *exec, const char *cwd)
{
    const char *base;

    if (!exec || !*exec) {
        return VAPOR_INST_NONE;
    }
    base = basename_of(exec);
    if (vapor_str_ends_with_ci(base, ".iso")
        || vapor_str_ends_with_ci(base, ".img")) {
        return VAPOR_INST_ISO;
    }
    if (vapor_str_ends_with_ci(base, ".msi")) {
        return VAPOR_INST_MSI;
    }
    if (cwd && *cwd) {
        if (sibling_exists(cwd, "setup.ini") || dir_has_product_msi(cwd)) {
            return VAPOR_INST_ISHIELD;
        }
    }
    if (vapor_str_has_prefix(base, "setup_")
        && vapor_str_ends_with_ci(base, ".exe")) {
        return VAPOR_INST_INNO;
    }
    if (file_has_ascii(exec, "Inno Setup") || file_has_ascii(exec, "InnoSetup")) {
        return VAPOR_INST_INNO;
    }
    if (file_has_ascii(exec, "NullsoftInst") || file_has_ascii(exec, "Nullsoft")) {
        return VAPOR_INST_NSIS;
    }
    if (file_has_ascii(exec, "InstallShield")) {
        return VAPOR_INST_ISHIELD;
    }
    if (vapor_str_ends_with_ci(base, ".exe")) {
        return VAPOR_INST_EXE;
    }
    return VAPOR_INST_NONE;
}

int
vapor_inst_silent_params(vapor_inst_kind kind, const char *exec, const char *dest,
                         char *out, size_t outsz)
{
    char d[VAPOR_WIN_PATH];

    if (!out || outsz == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!dest || !*dest) {
        return 1;
    }
    snprintf(d, sizeof(d), "%s", dest);
    vapor_plat_native_path(d);

    switch (kind) {
    case VAPOR_INST_INNO:
        /* GOG and most other Inno bootstrappers honor /DIR=. */
        if (snprintf(out, outsz,
                     "/VERYSILENT /NORESTART /SUPPRESSMSGBOXES /DIR=\"%s\"", d)
            >= (int)outsz) {
            return -1;
        }
        return 0;
    case VAPOR_INST_NSIS:
        /* /D= must be last and is not quoted. */
        if (snprintf(out, outsz, "/S /D=%s", d) >= (int)outsz) {
            return -1;
        }
        return 0;
    case VAPOR_INST_MSI:
        if (snprintf(out, outsz,
                     "/i \"%s\" /qn /norestart INSTALLDIR=\"%s\" TARGETDIR=\"%s\"",
                     exec, d, d)
            >= (int)outsz) {
            return -1;
        }
        return 0;
    case VAPOR_INST_ISHIELD:
        /* Disc InstallShield + MSI does not actually run unattended: /s /sms
         * /v"/qn ..." leaves setup.exe waiting on msiexec with no window.
         * Show the wizard and track the folder it creates. */
        return 1;
    default:
        return 1;
    }
}

static int
dirname_of(const char *path, char *out, size_t outsz)
{
    const char *base = basename_of(path);
    size_t      n;

    if (base == path) {
        return snprintf(out, outsz, ".") >= (int)outsz ? -1 : 0;
    }
    n = (size_t)(base - path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        n--;
    }
    if (n == 0) {
        return snprintf(out, outsz, ".") >= (int)outsz ? -1 : 0;
    }
    if (n >= outsz) {
        return -1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

int
vapor_exe_is_copy_protected(const char *path)
{
    char dir[VAPOR_WIN_PATH];

    if (!path || !*path || !vapor_plat_exists(path)) {
        return 0;
    }
    /* SafeDisc 2/3/4 stamps this cookie into the wrapper. Windows 10+
     * refuses to load SECDRV.SYS, so the wrapper then shows a fake
     * "administrator privileges" login instead of playing. */
    if (file_has_ascii(path, "BoG_")) {
        return 1;
    }
    if (dirname_of(path, dir, sizeof(dir)) == 0 && sibling_exists(dir, "SECDRV.SYS")) {
        return 1;
    }
    return 0;
}

static int
is_unprot_junk_name(const char *base)
{
    static const char *const junk[] = {
        "setup.exe", "install.exe", "autorun.exe", "launch.exe", "unins",
        "vcredist", "dxsetup.exe", "directx", "instmsi"
    };
    size_t i;

    for (i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        if (vapor_str_eq_ci(base, junk[i])
            || vapor_str_has_prefix(base, junk[i])) {
            return 1;
        }
    }
    if (vapor_str_has_prefix(base, "unins")
        && vapor_str_ends_with_ci(base, ".exe")) {
        return 1;
    }
    if (vapor_str_eq_ci(base, "upd.exe")
        || vapor_str_ends_with_ci(base, "up.exe")
        || vapor_str_ends_with_ci(base, "update.exe")
        || vapor_str_ends_with_ci(base, "ded.exe")) {
        return 1;
    }
    return 0;
}

typedef struct {
    char best[VAPOR_WIN_PATH];
    int  score;
    char want[VAPOR_ID_MAX + 1];
} unprot_scan;

static int
unprot_cb(const char *rel, const char *abs, void *ud)
{
    unprot_scan *s = ud;
    const char  *base;
    char         stem[VAPOR_ID_MAX + 1];
    int          score, depth;
    const char  *p;

    if (vapor_str_has_prefix(rel, "Setup/")
        || vapor_str_has_prefix(rel, "setup/")
        || vapor_str_has_prefix(rel, "DirectX/")
        || vapor_str_has_prefix(rel, "directx/")) {
        return 0;
    }
    if (!vapor_str_ends_with_ci(rel, ".exe") && !vapor_str_ends_with_ci(rel, ".bin")) {
        return 0;
    }
    base = basename_of(rel);
    if (is_unprot_junk_name(base)) {
        return 0;
    }
    if (vapor_exe_is_copy_protected(abs)) {
        return 0;
    }
    if (!s->want[0] || vapor_id_slug_stem(base, stem, sizeof(stem)) != 0
        || !vapor_slug_match(stem, s->want)) {
        return 0;
    }
    depth = 0;
    for (p = rel; *p; p++) {
        if (*p == '/' || *p == '\\') {
            depth++;
        }
    }
    score = 200 - depth * 15;
    if (score > s->score) {
        snprintf(s->best, sizeof(s->best), "%s", abs);
        s->score = score;
    }
    return 0;
}

int
vapor_find_unprotected_exe(const char *dir, const char *id, char *out,
                           size_t outsz)
{
    unprot_scan s;

    if (!dir || !*dir || !out || outsz == 0) {
        return 1;
    }
    memset(&s, 0, sizeof(s));
    s.score = -1;
    if (id && *id) {
        snprintf(s.want, sizeof(s.want), "%s", id);
    }
    vapor_plat_walk_files(dir, unprot_cb, &s);
    if (s.score < 0 || !s.best[0]) {
        return 1;
    }
    if ((size_t)snprintf(out, outsz, "%s", s.best) >= outsz) {
        return 1;
    }
    vapor_plat_native_path(out);
    return 0;
}
