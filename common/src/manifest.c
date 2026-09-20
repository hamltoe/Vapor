#include "vapor/manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "vapor/util.h"

const char *
vapor_host_platform(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

const char *
vapor_host_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

void
vapor_manifest_init(vapor_manifest *m)
{
    memset(m, 0, sizeof(*m));
    m->schema = VAPOR_MANIFEST_SCHEMA;
}

static void
free_str_array(char **arr, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(arr[i]);
    }
    free(arr);
}

static void
target_free(vapor_target *t)
{
    size_t i;

    free(t->platform);
    free(t->arch);
    free(t->runtime);
    free(t->exec);
    free(t->cwd);
    free_str_array(t->args, t->nargs);
    free_str_array(t->exec_bits, t->nexec_bits);
    for (i = 0; i < t->nenv; i++) {
        free(t->env[i].key);
        free(t->env[i].value);
    }
    free(t->env);
    memset(t, 0, sizeof(*t));
}

void
vapor_manifest_free(vapor_manifest *m)
{
    size_t i;

    if (!m) {
        return;
    }
    free(m->id);
    free(m->name);
    free(m->version);
    free(m->developer);
    free(m->description);
    free(m->cover);
    free(m->package.file);
    free(m->package.format);
    free(m->package.strip_prefix);
    for (i = 0; i < m->ntargets; i++) {
        target_free(&m->targets[i]);
    }
    free(m->targets);
    memset(m, 0, sizeof(*m));
}

static char *
json_dup_string(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return vapor_strdup(it->valuestring);
    }
    return NULL;
}

static int
json_dup_string_array(const cJSON *obj, const char *key,
                      char ***out, size_t *outn)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    const cJSON *it;
    size_t       n = 0, i = 0;
    char       **v;

    *out = NULL;
    *outn = 0;
    if (!arr || cJSON_IsNull(arr)) {
        return 0;
    }
    if (!cJSON_IsArray(arr)) {
        return -1;
    }

    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) {
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }

    v = (char **)calloc(n, sizeof(*v));
    if (!v) {
        return -1;
    }
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring) {
            continue;
        }
        v[i] = vapor_strdup(it->valuestring);
        if (!v[i]) {
            free_str_array(v, i);
            return -1;
        }
        i++;
    }
    *out = v;
    *outn = i;
    return 0;
}

static int
json_dup_env(const cJSON *obj, const char *key, vapor_kv **out, size_t *outn)
{
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(obj, key);
    const cJSON *it;
    size_t       n = 0, i = 0;
    vapor_kv    *v;

    *out = NULL;
    *outn = 0;
    if (!env || cJSON_IsNull(env)) {
        return 0;
    }
    if (!cJSON_IsObject(env)) {
        return -1;
    }

    cJSON_ArrayForEach(it, env) {
        if (cJSON_IsString(it) && it->string) {
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }

    v = (vapor_kv *)calloc(n, sizeof(*v));
    if (!v) {
        return -1;
    }
    cJSON_ArrayForEach(it, env) {
        if (!cJSON_IsString(it) || !it->string || !it->valuestring) {
            continue;
        }
        v[i].key = vapor_strdup(it->string);
        v[i].value = vapor_strdup(it->valuestring);
        if (!v[i].key || !v[i].value) {
            size_t k;
            for (k = 0; k <= i; k++) {
                free(v[k].key);
                free(v[k].value);
            }
            free(v);
            return -1;
        }
        i++;
    }
    *out = v;
    *outn = i;
    return 0;
}

static int
is_hex64(const char *s)
{
    int i;
    if (!s) {
        return 0;
    }
    for (i = 0; i < VAPOR_SHA256_HEX_LEN; i++) {
        char ch = s[i];
        int ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')
              || (ch >= 'A' && ch <= 'F');
        if (!ok) {
            return 0;
        }
    }
    return s[VAPOR_SHA256_HEX_LEN] == '\0';
}

#define FAIL(...)                                                             \
    do {                                                                      \
        if (err && errsz) {                                                   \
            snprintf(err, errsz, __VA_ARGS__);                                \
        }                                                                     \
        goto fail;                                                            \
    } while (0)

int
vapor_manifest_parse(const char *json, size_t len, vapor_manifest *out,
                     char *err, size_t errsz)
{
    cJSON        *root = NULL;
    const cJSON  *pkg, *targets, *it, *schema;
    size_t        ntargets = 0, i = 0;

    vapor_manifest_init(out);

    root = cJSON_ParseWithLength(json, len);
    if (!root) {
        FAIL("manifest is not valid JSON");
    }
    if (!cJSON_IsObject(root)) {
        FAIL("manifest root must be an object");
    }

    schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema)) {
        FAIL("manifest is missing \"schema\"");
    }
    out->schema = schema->valueint;
    if (out->schema != VAPOR_MANIFEST_SCHEMA) {
        FAIL("unsupported manifest schema %d (this build understands %d)",
             out->schema, VAPOR_MANIFEST_SCHEMA);
    }

    out->id          = json_dup_string(root, "id");
    out->name        = json_dup_string(root, "name");
    out->version     = json_dup_string(root, "version");
    out->developer   = json_dup_string(root, "developer");
    out->description = json_dup_string(root, "description");
    out->cover       = json_dup_string(root, "cover");

    if (!out->id || !vapor_id_is_valid(out->id)) {
        FAIL("\"id\" must be 1-%d chars of [a-z0-9._-] starting alphanumeric",
             VAPOR_ID_MAX);
    }
    if (!out->name || !*out->name) {
        FAIL("\"name\" is required");
    }
    if (!out->version || !*out->version) {
        FAIL("\"version\" is required");
    }
    /* Both this and package.file get appended to a server path, so neither may
     * contain a separator. */
    if (out->cover
        && (!*out->cover || strchr(out->cover, '/') || strchr(out->cover, '\\'))) {
        FAIL("\"cover\" must be a bare filename");
    }

    pkg = cJSON_GetObjectItemCaseSensitive(root, "package");
    if (!cJSON_IsObject(pkg)) {
        FAIL("\"package\" object is required");
    }
    out->package.file         = json_dup_string(pkg, "file");
    out->package.format       = json_dup_string(pkg, "format");
    out->package.strip_prefix = json_dup_string(pkg, "strip_prefix");
    if (!out->package.file || !*out->package.file) {
        FAIL("\"package.file\" is required");
    }
    if (strchr(out->package.file, '/') || strchr(out->package.file, '\\')) {
        FAIL("\"package.file\" must be a bare filename");
    }
    if (!out->package.format) {
        out->package.format = vapor_strdup("zip");
        if (!out->package.format) {
            FAIL("out of memory");
        }
    }
    if (strcmp(out->package.format, "zip") != 0
        && strcmp(out->package.format, "iso") != 0
        && strcmp(out->package.format, "file") != 0) {
        FAIL("unsupported package format \"%s\" (zip, iso, or file)",
             out->package.format);
    }
    {
        const cJSON *sz = cJSON_GetObjectItemCaseSensitive(pkg, "size");
        const cJSON *sh = cJSON_GetObjectItemCaseSensitive(pkg, "sha256");
        if (!cJSON_IsNumber(sz) || sz->valuedouble < 0) {
            FAIL("\"package.size\" must be a non-negative number");
        }
        out->package.size = (uint64_t)sz->valuedouble;
        if (!cJSON_IsString(sh) || !is_hex64(sh->valuestring)) {
            FAIL("\"package.sha256\" must be 64 hex characters");
        }
        memcpy(out->package.sha256, sh->valuestring, VAPOR_SHA256_HEX_LEN + 1);
    }

    targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    if (!cJSON_IsArray(targets)) {
        FAIL("\"targets\" array is required");
    }
    cJSON_ArrayForEach(it, targets) {
        if (cJSON_IsObject(it)) {
            ntargets++;
        }
    }
    if (ntargets == 0) {
        /* Disc images and zips whose launch files are found after extract. */
        out->targets = NULL;
        out->ntargets = 0;
        cJSON_Delete(root);
        return 0;
    }
    out->targets = (vapor_target *)calloc(ntargets, sizeof(*out->targets));
    if (!out->targets) {
        FAIL("out of memory");
    }

    cJSON_ArrayForEach(it, targets) {
        vapor_target *t;
        if (!cJSON_IsObject(it)) {
            continue;
        }
        t = &out->targets[i];
        t->platform = json_dup_string(it, "platform");
        t->arch     = json_dup_string(it, "arch");
        t->runtime  = json_dup_string(it, "runtime");
        t->exec     = json_dup_string(it, "exec");
        t->cwd      = json_dup_string(it, "cwd");
        if (json_dup_string_array(it, "args", &t->args, &t->nargs) != 0) {
            FAIL("target \"args\" must be an array of strings");
        }
        if (json_dup_string_array(it, "exec_bits",
                                  &t->exec_bits, &t->nexec_bits) != 0) {
            FAIL("target \"exec_bits\" must be an array of strings");
        }
        if (json_dup_env(it, "env", &t->env, &t->nenv) != 0) {
            FAIL("target \"env\" must be an object of string values");
        }
        if (!t->platform || !*t->platform) {
            FAIL("target %u is missing \"platform\"", (unsigned)i);
        }
        if (!t->exec || !*t->exec) {
            FAIL("target %u is missing \"exec\"", (unsigned)i);
        }
        /* exec is joined onto the install directory, so it must stay inside. */
        if (t->exec[0] == '/' || t->exec[0] == '\\' || strstr(t->exec, "..")) {
            FAIL("target %u \"exec\" must be a relative path without \"..\"",
                 (unsigned)i);
        }
        i++;
    }
    out->ntargets = i;

    cJSON_Delete(root);
    return 0;

fail:
    cJSON_Delete(root);
    vapor_manifest_free(out);
    return -1;
}

#undef FAIL

char *
vapor_manifest_serialize(const vapor_manifest *m)
{
    cJSON *root, *pkg, *targets;
    char  *text = NULL;
    size_t i, k;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    if (!cJSON_AddNumberToObject(root, "schema", m->schema)) { goto done; }
    if (!cJSON_AddStringToObject(root, "id", m->id ? m->id : "")) { goto done; }
    if (!cJSON_AddStringToObject(root, "name", m->name ? m->name : "")) { goto done; }
    if (!cJSON_AddStringToObject(root, "version", m->version ? m->version : "")) { goto done; }
    if (m->developer && !cJSON_AddStringToObject(root, "developer", m->developer)) { goto done; }
    if (m->description && !cJSON_AddStringToObject(root, "description", m->description)) { goto done; }
    if (m->cover && !cJSON_AddStringToObject(root, "cover", m->cover)) { goto done; }

    pkg = cJSON_AddObjectToObject(root, "package");
    if (!pkg) { goto done; }
    if (!cJSON_AddStringToObject(pkg, "file", m->package.file ? m->package.file : "")) { goto done; }
    if (!cJSON_AddStringToObject(pkg, "format", m->package.format ? m->package.format : "zip")) { goto done; }
    if (!cJSON_AddNumberToObject(pkg, "size", (double)m->package.size)) { goto done; }
    if (!cJSON_AddStringToObject(pkg, "sha256", m->package.sha256)) { goto done; }
    if (m->package.strip_prefix
        && !cJSON_AddStringToObject(pkg, "strip_prefix", m->package.strip_prefix)) {
        goto done;
    }

    targets = cJSON_AddArrayToObject(root, "targets");
    if (!targets) { goto done; }

    for (i = 0; i < m->ntargets; i++) {
        const vapor_target *t = &m->targets[i];
        cJSON *jt = cJSON_CreateObject();
        if (!jt) { goto done; }
        cJSON_AddItemToArray(targets, jt);

        if (!cJSON_AddStringToObject(jt, "platform", t->platform ? t->platform : "")) { goto done; }
        if (t->arch && !cJSON_AddStringToObject(jt, "arch", t->arch)) { goto done; }
        if (t->runtime && !cJSON_AddStringToObject(jt, "runtime", t->runtime)) { goto done; }
        if (!cJSON_AddStringToObject(jt, "exec", t->exec ? t->exec : "")) { goto done; }
        if (t->cwd && !cJSON_AddStringToObject(jt, "cwd", t->cwd)) { goto done; }

        if (t->nargs) {
            cJSON *a = cJSON_AddArrayToObject(jt, "args");
            if (!a) { goto done; }
            for (k = 0; k < t->nargs; k++) {
                cJSON *s = cJSON_CreateString(t->args[k]);
                if (!s) { goto done; }
                cJSON_AddItemToArray(a, s);
            }
        }
        if (t->nexec_bits) {
            cJSON *a = cJSON_AddArrayToObject(jt, "exec_bits");
            if (!a) { goto done; }
            for (k = 0; k < t->nexec_bits; k++) {
                cJSON *s = cJSON_CreateString(t->exec_bits[k]);
                if (!s) { goto done; }
                cJSON_AddItemToArray(a, s);
            }
        }
        if (t->nenv) {
            cJSON *e = cJSON_AddObjectToObject(jt, "env");
            if (!e) { goto done; }
            for (k = 0; k < t->nenv; k++) {
                if (!cJSON_AddStringToObject(e, t->env[k].key, t->env[k].value)) {
                    goto done;
                }
            }
        }
    }

    text = cJSON_Print(root);

done:
    cJSON_Delete(root);
    return text;
}

const vapor_target *
vapor_manifest_pick_target(const vapor_manifest *m, const char *platform,
                           const char *arch)
{
    size_t i;

    for (i = 0; i < m->ntargets; i++) {
        const vapor_target *t = &m->targets[i];
        if (vapor_str_eq_ci(t->platform, platform)
            && t->arch && vapor_str_eq_ci(t->arch, arch)) {
            return t;
        }
    }
    /* A target that does not pin an arch is treated as "any". */
    for (i = 0; i < m->ntargets; i++) {
        const vapor_target *t = &m->targets[i];
        if (vapor_str_eq_ci(t->platform, platform) && !t->arch) {
            return t;
        }
    }
    return NULL;
}
