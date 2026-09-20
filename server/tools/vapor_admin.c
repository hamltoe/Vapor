/* vapor-admin: packages a game directory into the content root and registers it
 * in the catalog. Runs on the server, next to the content it manages.
 *
 *   vapor-admin add ./HollowVale --id hollow-vale --name "Hollow Vale" \
 *       --version 1.0.3 --linux-exec bin/hollowvale --exec-bit bin/hollowvale
 */

#include "vapord.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "miniz.h"
#include "vapor/buf.h"
#include "vapor/util.h"

#define PACKAGE_FILENAME "package.zip"

/* ------------------------------------------------------- growable string list */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strlist;

static int
strlist_push(strlist *l, const char *s)
{
    if (l->count == l->cap) {
        size_t newcap = l->cap ? l->cap * 2 : 8;
        char **grown = (char **)realloc(l->items, newcap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        l->items = grown;
        l->cap = newcap;
    }
    l->items[l->count] = vapor_strdup(s);
    if (!l->items[l->count]) {
        return -1;
    }
    l->count++;
    return 0;
}

static void
strlist_free(strlist *l)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        free(l->items[i]);
    }
    free(l->items);
    memset(l, 0, sizeof(*l));
}

/* ---------------------------------------------------------------- file copy */

static int
copy_file(const char *src, const char *dst)
{
    FILE  *in, *out;
    char   buf[64 * 1024];
    size_t n;
    int    rc = 0;

    in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) {
        rc = -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        rc = -1;
    }
    return rc;
}

/* ------------------------------------------------------------- zip packaging */

typedef struct {
    mz_zip_archive zip;
    size_t         nfiles;
    uint64_t       nbytes;
} packer;

static int
add_directory(packer *p, const char *src_root, const char *rel)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if ((size_t)snprintf(abs, sizeof(abs), "%s%s%s", src_root,
                         *rel ? "/" : "", rel)
        >= sizeof(abs)) {
        fprintf(stderr, "vapor-admin: path too long under %s\n", rel);
        return -1;
    }

    d = opendir(abs);
    if (!d) {
        fprintf(stderr, "vapor-admin: cannot read directory %s\n", abs);
        return -1;
    }

    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                             *rel ? "/" : "", ent->d_name)
            >= sizeof(child_rel)) {
            rc = -1;
            break;
        }
        if ((size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", src_root,
                             child_rel)
            >= sizeof(child_abs)) {
            rc = -1;
            break;
        }
        /* lstat, not stat: a symlink would otherwise be followed and could pull
         * content in from outside the source tree. */
        if (lstat(child_abs, &st) != 0) {
            fprintf(stderr, "vapor-admin: cannot stat %s\n", child_abs);
            rc = -1;
            break;
        }

        if (S_ISLNK(st.st_mode)) {
            fprintf(stderr, "vapor-admin: skipping symlink %s (zip cannot "
                            "represent it)\n", child_rel);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char dir_entry[VAPORD_PATH_MAX + 2];
            /* A trailing slash records the directory itself, which preserves
             * empty directories that a game may expect to exist. */
            snprintf(dir_entry, sizeof(dir_entry), "%s/", child_rel);
            if (!mz_zip_writer_add_mem(&p->zip, dir_entry, NULL, 0, 0)) {
                fprintf(stderr, "vapor-admin: cannot add directory %s\n",
                        child_rel);
                rc = -1;
                break;
            }
            if (add_directory(p, src_root, child_rel) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "vapor-admin: skipping non-regular file %s\n",
                    child_rel);
            continue;
        }

        if (!mz_zip_writer_add_file(&p->zip, child_rel, child_abs, NULL, 0,
                                    MZ_DEFAULT_LEVEL)) {
            fprintf(stderr, "vapor-admin: cannot add %s\n", child_rel);
            rc = -1;
            break;
        }
        p->nfiles++;
        p->nbytes += (uint64_t)st.st_size;
    }

    closedir(d);
    return rc;
}

static int
file_size(const char *path, uint64_t *out)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

static int
build_package(const char *src_dir, const char *out_zip, size_t *out_files,
              uint64_t *out_raw)
{
    packer p;

    memset(&p, 0, sizeof(p));
    if (!mz_zip_writer_init_file(&p.zip, out_zip, 0)) {
        fprintf(stderr, "vapor-admin: cannot create %s\n", out_zip);
        return -1;
    }

    if (add_directory(&p, src_dir, "") != 0) {
        mz_zip_writer_end(&p.zip);
        remove(out_zip);
        return -1;
    }
    if (p.nfiles == 0) {
        fprintf(stderr, "vapor-admin: %s contains no files\n", src_dir);
        mz_zip_writer_end(&p.zip);
        remove(out_zip);
        return -1;
    }

    if (!mz_zip_writer_finalize_archive(&p.zip)) {
        fprintf(stderr, "vapor-admin: cannot finalize %s\n", out_zip);
        mz_zip_writer_end(&p.zip);
        remove(out_zip);
        return -1;
    }
    mz_zip_writer_end(&p.zip);

    *out_files = p.nfiles;
    *out_raw = p.nbytes;
    return 0;
}

/* --------------------------------------------------------------- add command */

static void
add_usage(void)
{
    printf("usage: vapor-admin add DIR --id ID --name NAME --version VERSION "
           "[options]\n\n");
    printf("  --id ID                catalog id: [a-z0-9._-], starts alphanumeric\n");
    printf("  --name NAME            display name\n");
    printf("  --version VERSION      version string, compared naturally\n");
    printf("  --developer NAME       optional\n");
    printf("  --description TEXT     optional\n");
    printf("  --cover FILE           .png or .jpg cover art for the library view\n");
    printf("  --linux-exec PATH      relative path to the Linux executable\n");
    printf("  --windows-exec PATH    relative path to the Windows executable\n");
    printf("  --arch ARCH            target arch (default x86_64)\n");
    printf("  --cwd PATH             working directory, relative to install dir\n");
    printf("  --arg ARG              launch argument (repeatable)\n");
    printf("  --env KEY=VALUE        environment variable (repeatable)\n");
    printf("  --exec-bit GLOB        chmod +x after extract, Linux (repeatable)\n");
    printf("  --strip-prefix PREFIX  drop this leading path on extract\n\n");
    printf("At least one of --linux-exec or --windows-exec is required.\n");
    printf("$INSTALL_DIR in --env and --arg is expanded by the client at launch.\n");
}

static int
cmd_add(vapord *app, int argc, char **argv)
{
    const char *src_dir = NULL, *id = NULL, *name = NULL, *version = NULL;
    const char *developer = NULL, *description = NULL, *cover = NULL;
    const char *linux_exec = NULL, *windows_exec = NULL;
    const char *arch = "x86_64", *cwd = NULL, *strip_prefix = NULL;
    strlist     args = { 0 }, envs = { 0 }, exec_bits = { 0 };
    struct {
        const char *platform;
        const char *exec;
    } target_specs[2] = { { 0 } };
    vapor_manifest m;
    char        version_dir[VAPORD_PATH_MAX];
    char        zip_path[VAPORD_PATH_MAX];
    char        manifest_path[VAPORD_PATH_MAX];
    char        cover_name[64] = "";
    char        cover_path[VAPORD_PATH_MAX];
    char       *manifest_json = NULL;
    char        sha[VAPOR_SHA256_HEX_LEN + 1];
    char        pretty[32], pretty_raw[32];
    struct stat st;
    size_t      nfiles = 0, ntargets = 0, ti = 0;
    uint64_t    zip_size = 0, raw_size = 0;
    int         i, rc = 1;
    FILE       *f;

    vapor_manifest_init(&m);

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        int         has_next = (i + 1 < argc);

#define TAKE(flag, dest)                                                       \
    if (strcmp(a, flag) == 0) {                                                \
        if (!has_next) {                                                       \
            fprintf(stderr, "vapor-admin: %s needs a value\n", flag);           \
            goto done;                                                         \
        }                                                                      \
        dest = argv[++i];                                                      \
        continue;                                                              \
    }

        TAKE("--id", id)
        TAKE("--name", name)
        TAKE("--version", version)
        TAKE("--developer", developer)
        TAKE("--description", description)
        TAKE("--cover", cover)
        TAKE("--linux-exec", linux_exec)
        TAKE("--windows-exec", windows_exec)
        TAKE("--arch", arch)
        TAKE("--cwd", cwd)
        TAKE("--strip-prefix", strip_prefix)
#undef TAKE

        if (strcmp(a, "--arg") == 0 || strcmp(a, "--env") == 0
            || strcmp(a, "--exec-bit") == 0) {
            strlist *target = (strcmp(a, "--arg") == 0)      ? &args
                              : (strcmp(a, "--env") == 0)    ? &envs
                                                             : &exec_bits;
            if (!has_next) {
                fprintf(stderr, "vapor-admin: %s needs a value\n", a);
                goto done;
            }
            if (strlist_push(target, argv[++i]) != 0) {
                fprintf(stderr, "vapor-admin: out of memory\n");
                goto done;
            }
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            add_usage();
            rc = 0;
            goto done;
        }
        if (a[0] == '-') {
            fprintf(stderr, "vapor-admin: unknown option \"%s\"\n", a);
            goto done;
        }
        if (src_dir) {
            fprintf(stderr, "vapor-admin: more than one source directory given\n");
            goto done;
        }
        src_dir = a;
    }

    if (!src_dir || !id || !name || !version) {
        fprintf(stderr, "vapor-admin: DIR, --id, --name and --version are "
                        "required\n\n");
        add_usage();
        goto done;
    }
    if (!vapor_id_is_valid(id)) {
        fprintf(stderr, "vapor-admin: invalid --id \"%s\": use 1-%d chars of "
                        "[a-z0-9._-] starting alphanumeric\n", id, VAPOR_ID_MAX);
        goto done;
    }
    if (!vapor_version_is_valid(version)) {
        fprintf(stderr, "vapor-admin: invalid --version \"%s\"\n", version);
        goto done;
    }
    if (!linux_exec && !windows_exec) {
        fprintf(stderr, "vapor-admin: at least one of --linux-exec or "
                        "--windows-exec is required\n");
        goto done;
    }
    if (stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "vapor-admin: \"%s\" is not a directory\n", src_dir);
        goto done;
    }
    /* Covers are stored under a fixed name so the manifest never has to carry a
     * path the publisher chose; only the extension survives, because that is
     * what tells the client which decoder to use. */
    if (cover) {
        if (vapor_str_ends_with_ci(cover, ".png")) {
            snprintf(cover_name, sizeof(cover_name), "cover.png");
        } else if (vapor_str_ends_with_ci(cover, ".jpg")
                   || vapor_str_ends_with_ci(cover, ".jpeg")) {
            snprintf(cover_name, sizeof(cover_name), "cover.jpg");
        } else {
            fprintf(stderr, "vapor-admin: --cover must be .png, .jpg or .jpeg\n");
            goto done;
        }
        if (stat(cover, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "vapor-admin: cannot read cover \"%s\"\n", cover);
            goto done;
        }
        if ((uint64_t)st.st_size > VAPOR_MAX_COVER_BYTES) {
            fprintf(stderr, "vapor-admin: cover is larger than the %d MiB the "
                            "client will fetch\n",
                    VAPOR_MAX_COVER_BYTES / (1024 * 1024));
            goto done;
        }
    }

    /* Validate env entries before doing any expensive work. */
    for (i = 0; i < (int)envs.count; i++) {
        if (!strchr(envs.items[i], '=')) {
            fprintf(stderr, "vapor-admin: --env \"%s\" must be KEY=VALUE\n",
                    envs.items[i]);
            goto done;
        }
    }

    if (vapord_content_path(&app->cfg, id, version, NULL, version_dir,
                            sizeof(version_dir))
        != 0) {
        fprintf(stderr, "vapor-admin: cannot build a content path for %s/%s\n",
                id, version);
        goto done;
    }
    if (vapord_content_mkdirs(version_dir) != 0) {
        fprintf(stderr, "vapor-admin: cannot create %s\n", version_dir);
        goto done;
    }
    if ((size_t)snprintf(zip_path, sizeof(zip_path), "%s/%s", version_dir,
                         PACKAGE_FILENAME)
            >= sizeof(zip_path)
        || (size_t)snprintf(manifest_path, sizeof(manifest_path),
                            "%s/manifest.json", version_dir)
               >= sizeof(manifest_path)) {
        fprintf(stderr, "vapor-admin: the content path for %s/%s is too long\n",
                id, version);
        goto done;
    }

    if (cover_name[0]) {
        if ((size_t)snprintf(cover_path, sizeof(cover_path), "%s/%s", version_dir,
                             cover_name)
            >= sizeof(cover_path)) {
            fprintf(stderr, "vapor-admin: the cover path for %s/%s is too long\n",
                    id, version);
            goto done;
        }
        if (copy_file(cover, cover_path) != 0) {
            fprintf(stderr, "vapor-admin: cannot copy %s to %s\n", cover,
                    cover_path);
            goto done;
        }
    }

    printf("packaging %s\n", src_dir);
    if (build_package(src_dir, zip_path, &nfiles, &raw_size) != 0) {
        goto done;
    }
    if (file_size(zip_path, &zip_size) != 0) {
        fprintf(stderr, "vapor-admin: cannot size %s\n", zip_path);
        goto done;
    }
    if (vapor_sha256_file(zip_path, sha) != 0) {
        fprintf(stderr, "vapor-admin: cannot hash %s\n", zip_path);
        goto done;
    }

    vapor_format_bytes(zip_size, pretty, sizeof(pretty));
    vapor_format_bytes(raw_size, pretty_raw, sizeof(pretty_raw));
    printf("  %zu file(s), %s raw -> %s compressed\n", nfiles, pretty_raw, pretty);
    printf("  sha256 %s\n", sha);

    /* Assemble the manifest. */
    m.schema = VAPOR_MANIFEST_SCHEMA;
    m.id = vapor_strdup(id);
    m.name = vapor_strdup(name);
    m.version = vapor_strdup(version);
    m.developer = developer ? vapor_strdup(developer) : NULL;
    m.description = description ? vapor_strdup(description) : NULL;
    m.cover = cover_name[0] ? vapor_strdup(cover_name) : NULL;
    m.package.file = vapor_strdup(PACKAGE_FILENAME);
    m.package.format = vapor_strdup("zip");
    m.package.size = zip_size;
    m.package.strip_prefix = strip_prefix ? vapor_strdup(strip_prefix) : NULL;
    memcpy(m.package.sha256, sha, sizeof(sha));

    if (!m.id || !m.name || !m.version || !m.package.file || !m.package.format) {
        fprintf(stderr, "vapor-admin: out of memory\n");
        goto done;
    }

    /* One target per platform the caller supplied an executable for. */
    {
        size_t k = 0;
        if (linux_exec) {
            target_specs[k].platform = "linux";
            target_specs[k].exec = linux_exec;
            k++;
        }
        if (windows_exec) {
            target_specs[k].platform = "windows";
            target_specs[k].exec = windows_exec;
            k++;
        }
        ntargets = k;
    }

    m.targets = (vapor_target *)calloc(ntargets, sizeof(*m.targets));
    if (!m.targets) {
        fprintf(stderr, "vapor-admin: out of memory\n");
        goto done;
    }
    m.ntargets = ntargets;

    for (ti = 0; ti < ntargets; ti++) {
        vapor_target *t = &m.targets[ti];
        int           is_linux = strcmp(target_specs[ti].platform, "linux") == 0;

        t->platform = vapor_strdup(target_specs[ti].platform);
        t->arch = vapor_strdup(arch);
        t->exec = vapor_strdup(target_specs[ti].exec);
        t->cwd = cwd ? vapor_strdup(cwd) : NULL;
        if (!t->platform || !t->arch || !t->exec) {
            fprintf(stderr, "vapor-admin: out of memory\n");
            goto done;
        }

        /* args and env apply to every target; exec_bits only mean something on
         * Linux, where zip does not carry the executable bit. */
        if (args.count) {
            size_t k;
            t->args = (char **)calloc(args.count, sizeof(*t->args));
            if (!t->args) { goto done; }
            for (k = 0; k < args.count; k++) {
                t->args[k] = vapor_strdup(args.items[k]);
                if (!t->args[k]) { goto done; }
            }
            t->nargs = args.count;
        }
        if (envs.count) {
            size_t k;
            t->env = (vapor_kv *)calloc(envs.count, sizeof(*t->env));
            if (!t->env) { goto done; }
            for (k = 0; k < envs.count; k++) {
                char *eq = strchr(envs.items[k], '=');
                *eq = '\0';
                t->env[k].key = vapor_strdup(envs.items[k]);
                t->env[k].value = vapor_strdup(eq + 1);
                *eq = '=';
                if (!t->env[k].key || !t->env[k].value) { goto done; }
            }
            t->nenv = envs.count;
        }
        if (is_linux && exec_bits.count) {
            size_t k;
            t->exec_bits = (char **)calloc(exec_bits.count, sizeof(*t->exec_bits));
            if (!t->exec_bits) { goto done; }
            for (k = 0; k < exec_bits.count; k++) {
                t->exec_bits[k] = vapor_strdup(exec_bits.items[k]);
                if (!t->exec_bits[k]) { goto done; }
            }
            t->nexec_bits = exec_bits.count;
        }
    }

    manifest_json = vapor_manifest_serialize(&m);
    if (!manifest_json) {
        fprintf(stderr, "vapor-admin: cannot serialize the manifest\n");
        goto done;
    }

    /* Round-trip through the parser: if the client would reject this manifest,
     * fail here rather than shipping something uninstallable. */
    {
        vapor_manifest check;
        char           err[256];
        if (vapor_manifest_parse(manifest_json, strlen(manifest_json), &check,
                                 err, sizeof(err))
            != 0) {
            fprintf(stderr, "vapor-admin: generated manifest is invalid: %s\n",
                    err);
            goto done;
        }
        vapor_manifest_free(&check);
    }

    f = fopen(manifest_path, "w");
    if (!f) {
        fprintf(stderr, "vapor-admin: cannot write %s\n", manifest_path);
        goto done;
    }
    fputs(manifest_json, f);
    fputc('\n', f);
    fclose(f);

    if (vapord_game_upsert(app->db, &m) != 0
        || vapord_version_upsert(app->db, &m, manifest_json) != 0
        || vapord_game_mark_discovered(app->db, id, 0) != 0) {
        fprintf(stderr, "vapor-admin: cannot register %s in the catalog\n", id);
        goto done;
    }

    printf("registered %s version %s\n", id, version);
    printf("  content  %s\n", version_dir);
    if (m.cover) {
        printf("  cover    %s\n", m.cover);
    }
    for (ti = 0; ti < m.ntargets; ti++) {
        printf("  target   %s/%s -> %s\n", m.targets[ti].platform,
               m.targets[ti].arch, m.targets[ti].exec);
    }
    rc = 0;

done:
    free(manifest_json);
    vapor_manifest_free(&m);
    strlist_free(&args);
    strlist_free(&envs);
    strlist_free(&exec_bits);
    return rc;
}

/* -------------------------------------------------------------- other commands */

static int
cmd_list(vapord *app)
{
    cJSON *catalog = vapord_catalog_json(app->db);
    cJSON *games, *g;
    int    n = 0;

    if (!catalog) {
        fprintf(stderr, "vapor-admin: cannot read the catalog\n");
        return 1;
    }
    games = cJSON_GetObjectItemCaseSensitive(catalog, "games");
    cJSON_ArrayForEach(g, games) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(g, "id");
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(g, "name");
        const cJSON *lv = cJSON_GetObjectItemCaseSensitive(g, "latest_version");
        const cJSON *sz = cJSON_GetObjectItemCaseSensitive(g, "size");
        char         pretty[32];

        vapor_format_bytes(cJSON_IsNumber(sz) ? (uint64_t)sz->valuedouble : 0,
                           pretty, sizeof(pretty));
        printf("%-24s %-12s %10s  %s\n",
               cJSON_IsString(id) ? id->valuestring : "?",
               cJSON_IsString(lv) ? lv->valuestring : "-", pretty,
               cJSON_IsString(nm) ? nm->valuestring : "");
        n++;
    }
    if (n == 0) {
        printf("catalog is empty\n");
    }
    cJSON_Delete(catalog);
    return 0;
}

static int
cmd_remove(vapord *app, const char *id)
{
    char path[VAPORD_PATH_MAX];
    int  rc;

    if (!vapor_id_is_valid(id)) {
        fprintf(stderr, "vapor-admin: invalid id \"%s\"\n", id);
        return 1;
    }
    rc = vapord_game_delete(app->db, id);
    if (rc < 0) {
        fprintf(stderr, "vapor-admin: database error\n");
        return 1;
    }
    if (rc == 1) {
        fprintf(stderr, "vapor-admin: no such game \"%s\"\n", id);
        return 1;
    }

    /* The catalog row is gone; leave the bytes in place so a mistaken remove is
     * recoverable by re-running add. */
    if (vapord_content_path(&app->cfg, id, NULL, NULL, path, sizeof(path)) == 0) {
        printf("removed \"%s\" from the catalog\n", id);
        printf("content is still on disk at %s (delete it by hand if you want "
               "the space back)\n", path);
    }
    return 0;
}

static int
cmd_users(vapord *app)
{
    int64_t count = 0;

    if (vapord_user_count(app->db, &count) != 0) {
        fprintf(stderr, "vapor-admin: cannot count users\n");
        return 1;
    }
    printf("%lld account(s)\n", (long long)count);
    return 0;
}

static void
usage(void)
{
    printf("vapor-admin %s - manage the Vapor content root and catalog\n\n",
           VAPOR_VERSION_STRING);
    printf("usage: vapor-admin [-c CONFIG] [-r CONTENT_ROOT] [-L LIBRARY_ROOT] "
           "[-d DB] COMMAND\n\n");
    printf("  add DIR [options]   package and register a game (see add --help)\n");
    printf("  discover            scan library_root and register game folders\n");
    printf("  list                show the catalog\n");
    printf("  remove ID           unregister a game\n");
    printf("  users               count registered accounts\n\n");
}

int
main(int argc, char **argv)
{
    vapord      app;
    char        err[512];
    const char *cfg_path = NULL;
    int         i, argi = 1, rc;

    memset(&app, 0, sizeof(app));
    vapord_config_defaults(&app.cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
            argi = i + 1;
        } else {
            break;
        }
    }
    if (cfg_path && vapord_config_load(&app.cfg, cfg_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "vapor-admin: %s\n", err);
        return 1;
    }

    for (i = argi; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            snprintf(app.cfg.content_root, sizeof(app.cfg.content_root), "%s",
                     argv[++i]);
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            snprintf(app.cfg.library_root, sizeof(app.cfg.library_root), "%s",
                     argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            snprintf(app.cfg.db_path, sizeof(app.cfg.db_path), "%s", argv[++i]);
        } else {
            break;
        }
    }
    argi = i;

    if (argi >= argc) {
        usage();
        return 1;
    }

    if (vapord_auth_init(err, sizeof(err)) != 0) {
        fprintf(stderr, "vapor-admin: %s\n", err);
        return 1;
    }
    if (vapord_db_open(&app, err, sizeof(err)) != 0) {
        fprintf(stderr, "vapor-admin: %s\n", err);
        return 1;
    }

    if (strcmp(argv[argi], "add") == 0) {
        rc = cmd_add(&app, argc - argi - 1, argv + argi + 1);
    } else if (strcmp(argv[argi], "discover") == 0) {
        if (!app.cfg.library_root[0]) {
            fprintf(stderr, "vapor-admin: set library_root in the config or pass -L\n");
            rc = 1;
        } else if (vapord_discover(&app) != 0) {
            rc = 1;
        } else {
            rc = cmd_list(&app);
        }
    } else if (strcmp(argv[argi], "list") == 0) {
        rc = cmd_list(&app);
    } else if (strcmp(argv[argi], "remove") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "vapor-admin: remove needs a game id\n");
            rc = 1;
        } else {
            rc = cmd_remove(&app, argv[argi + 1]);
        }
    } else if (strcmp(argv[argi], "users") == 0) {
        rc = cmd_users(&app);
    } else if (strcmp(argv[argi], "-h") == 0
               || strcmp(argv[argi], "--help") == 0) {
        usage();
        rc = 0;
    } else {
        fprintf(stderr, "vapor-admin: unknown command \"%s\"\n\n", argv[argi]);
        usage();
        rc = 1;
    }

    vapord_db_close(&app);
    return rc;
}
