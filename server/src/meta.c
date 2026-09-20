/* Fill empty catalog fields from the Steam store: cover art, description,
 * developer, and the public review score. Failures are warnings; discovery
 * still publishes the game. Lookups are cached on the game row so a scan
 * does not hammer Steam every minute. */

#include "vapord.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "vapor/buf.h"
#include "vapor/protocol.h"
#include "vapor/util.h"

#define META_JSON_MAX   (2 * 1024 * 1024)
#define META_CACHE_SECS (7 * 24 * 3600)

typedef struct {
    vapor_buf buf;
    size_t    max_bytes;
    int       overflow;
} mem_sink;

typedef struct {
    FILE   *f;
    size_t  max_bytes;
    size_t  wrote;
    int     overflow;
} file_sink;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

static void
curl_init_once(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void
ensure_curl(void)
{
    pthread_once(&curl_once, curl_init_once);
}

static size_t
on_mem(char *ptr, size_t size, size_t nmemb, void *ud)
{
    mem_sink *s = (mem_sink *)ud;
    size_t    n = size * nmemb;

    if (s->buf.len + n > s->max_bytes) {
        s->overflow = 1;
        return 0;
    }
    return vapor_buf_append(&s->buf, ptr, n) == 0 ? n : 0;
}

static size_t
on_file(char *ptr, size_t size, size_t nmemb, void *ud)
{
    file_sink *s = (file_sink *)ud;
    size_t     n = size * nmemb;

    if (s->wrote + n > s->max_bytes) {
        s->overflow = 1;
        return 0;
    }
    if (fwrite(ptr, 1, n, s->f) != n) {
        return 0;
    }
    s->wrote += n;
    return n;
}

static CURL *
easy_setup(const char *url)
{
    CURL *c = curl_easy_init();

    if (!c) {
        return NULL;
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     "Vapor/" VAPOR_VERSION_STRING " (self-hosted library)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    return c;
}

static int
http_get(const char *url, char **out, size_t *outlen, size_t max_bytes,
         long *status)
{
    CURL     *c;
    CURLcode  rc;
    mem_sink  sink;
    long      code = 0;

    *out = NULL;
    if (outlen) {
        *outlen = 0;
    }
    if (status) {
        *status = 0;
    }

    ensure_curl();
    c = easy_setup(url);
    if (!c) {
        return -1;
    }

    memset(&sink, 0, sizeof(sink));
    vapor_buf_init(&sink.buf);
    sink.max_bytes = max_bytes;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_bytes);

    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (status) {
        *status = code;
    }
    if (rc != CURLE_OK || sink.overflow || code < 200 || code > 299) {
        vapor_buf_free(&sink.buf);
        return -1;
    }
    if (outlen) {
        *outlen = sink.buf.len;
    }
    *out = vapor_buf_release(&sink.buf);
    return *out ? 0 : -1;
}

static int
http_get_file(const char *url, const char *path, size_t max_bytes)
{
    CURL      *c;
    CURLcode   rc;
    FILE      *f;
    file_sink  sink;
    long       code = 0;

    ensure_curl();
    f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    c = easy_setup(url);
    if (!c) {
        fclose(f);
        remove(path);
        return -1;
    }

    memset(&sink, 0, sizeof(sink));
    sink.f = f;
    sink.max_bytes = max_bytes;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_bytes);

    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (fclose(f) != 0) {
        remove(path);
        return -1;
    }
    if (rc != CURLE_OK || sink.overflow || code < 200 || code > 299
        || sink.wrote == 0) {
        remove(path);
        return -1;
    }
    return 0;
}

static void
url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t            n = 0;

    if (outsz == 0) {
        return;
    }
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else if (n + 3 < outsz) {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        } else {
            break;
        }
    }
    out[n] = '\0';
}

static void
fold_name(const char *in, char *out, size_t outsz)
{
    size_t n = 0;

    if (outsz == 0) {
        return;
    }
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
}

static void
strip_html(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int    in_tag = 0;
    int    space = 1;

    if (outsz == 0) {
        return;
    }
    for (; *in && n + 1 < outsz; in++) {
        if (*in == '<') {
            in_tag = 1;
            continue;
        }
        if (in_tag) {
            if (*in == '>') {
                in_tag = 0;
            }
            continue;
        }
        if (*in == '&') {
            if (strncmp(in, "&amp;", 5) == 0) {
                out[n++] = '&';
                in += 4;
            } else if (strncmp(in, "&lt;", 4) == 0) {
                out[n++] = '<';
                in += 3;
            } else if (strncmp(in, "&gt;", 4) == 0) {
                out[n++] = '>';
                in += 3;
            } else if (strncmp(in, "&quot;", 6) == 0) {
                out[n++] = '"';
                in += 5;
            } else if (strncmp(in, "&#39;", 5) == 0
                       || strncmp(in, "&apos;", 6) == 0) {
                out[n++] = '\'';
                in += (*in == '&' && in[1] == '#') ? 4 : 5;
            } else if (strncmp(in, "&nbsp;", 6) == 0) {
                if (!space && n + 1 < outsz) {
                    out[n++] = ' ';
                    space = 1;
                }
                in += 5;
            } else {
                /* Skip the rest of the entity. */
                while (*in && *in != ';') {
                    in++;
                }
                if (!*in) {
                    break;
                }
            }
            continue;
        }
        if (isspace((unsigned char)*in)) {
            if (!space && n + 1 < outsz) {
                out[n++] = ' ';
                space = 1;
            }
            continue;
        }
        out[n++] = *in;
        space = 0;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

static int
json_appid(const cJSON *obj)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "id");

    if (!cJSON_IsNumber(v)) {
        v = cJSON_GetObjectItemCaseSensitive(obj, "appid");
    }
    if (cJSON_IsNumber(v) && v->valuedouble > 0) {
        return (int)v->valuedouble;
    }
    return 0;
}

static int
search_steam(const char *name, int *out_appid)
{
    char   q[512], url[768];
    char  *body = NULL;
    cJSON *root, *items, *it;
    char   want[128];
    int    best = 0, best_exact = 0;

    *out_appid = 0;
    url_encode(name, q, sizeof(q));
    if (!q[0]) {
        return 1;
    }
    snprintf(url, sizeof(url),
             "https://store.steampowered.com/api/storesearch/?term=%s&l=english&cc=US",
             q);
    if (http_get(url, &body, NULL, META_JSON_MAX, NULL) != 0) {
        return -1;
    }
    root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return -1;
    }

    fold_name(name, want, sizeof(want));
    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    cJSON_ArrayForEach(it, items) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
        char         have[128];
        int          id;

        if (!cJSON_IsString(nm) || !nm->valuestring) {
            continue;
        }
        if (cJSON_IsString(ty) && ty->valuestring
            && strcmp(ty->valuestring, "app") != 0) {
            continue;
        }
        id = json_appid(it);
        if (id <= 0) {
            continue;
        }
        fold_name(nm->valuestring, have, sizeof(have));
        if (want[0] && strcmp(have, want) == 0) {
            best = id;
            best_exact = 1;
            break;
        }
        if (!best) {
            best = id;
        }
    }
    cJSON_Delete(root);

    if (!best) {
        return 1;
    }
    /* A non-exact first hit is still used: folder names like "Half Life GOTY"
     * rarely match the store title byte-for-byte, and the top search result is
     * usually the right app. */
    (void)best_exact;
    *out_appid = best;
    return 0;
}

static int
fetch_details(int appid, char *developer, size_t devsz, char *description,
              size_t descsz, char *header_url, size_t urlsz)
{
    char   url[256];
    char  *body = NULL;
    cJSON *root, *entry, *data, *devs, *first;
    char   key[32];

    if (developer && devsz) {
        developer[0] = '\0';
    }
    if (description && descsz) {
        description[0] = '\0';
    }
    if (header_url && urlsz) {
        header_url[0] = '\0';
    }

    snprintf(url, sizeof(url),
             "https://store.steampowered.com/api/appdetails?appids=%d&l=english",
             appid);
    if (http_get(url, &body, NULL, META_JSON_MAX, NULL) != 0) {
        return -1;
    }
    root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return -1;
    }

    snprintf(key, sizeof(key), "%d", appid);
    entry = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(entry)
        || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "success"))) {
        cJSON_Delete(root);
        return 1;
    }
    data = cJSON_GetObjectItemCaseSensitive(entry, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return 1;
    }

    {
        const cJSON *short_d =
            cJSON_GetObjectItemCaseSensitive(data, "short_description");
        const cJSON *about =
            cJSON_GetObjectItemCaseSensitive(data, "about_the_game");
        const char *raw = NULL;

        if (cJSON_IsString(short_d) && short_d->valuestring
            && short_d->valuestring[0]) {
            raw = short_d->valuestring;
        } else if (cJSON_IsString(about) && about->valuestring) {
            raw = about->valuestring;
        }
        if (raw && description && descsz) {
            strip_html(raw, description, descsz);
        }
    }

    devs = cJSON_GetObjectItemCaseSensitive(data, "developers");
    first = cJSON_IsArray(devs) ? cJSON_GetArrayItem(devs, 0) : NULL;
    if (cJSON_IsString(first) && first->valuestring && developer && devsz) {
        snprintf(developer, devsz, "%s", first->valuestring);
    }

    {
        const cJSON *hdr =
            cJSON_GetObjectItemCaseSensitive(data, "header_image");
        if (cJSON_IsString(hdr) && hdr->valuestring && header_url && urlsz) {
            snprintf(header_url, urlsz, "%s", hdr->valuestring);
        }
    }

    cJSON_Delete(root);
    return 0;
}

static int
fetch_reviews(int appid, int *pct, int *count, char *label, size_t labelsz)
{
    char   url[256];
    char  *body = NULL;
    cJSON *root, *sum;

    *pct = -1;
    *count = -1;
    if (label && labelsz) {
        label[0] = '\0';
    }

    snprintf(url, sizeof(url),
             "https://store.steampowered.com/appreviews/%d"
             "?json=1&language=all&purchase_type=all&num_per_page=0",
             appid);
    if (http_get(url, &body, NULL, META_JSON_MAX, NULL) != 0) {
        return -1;
    }
    root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return -1;
    }
    sum = cJSON_GetObjectItemCaseSensitive(root, "query_summary");
    if (cJSON_IsObject(sum)) {
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(sum, "total_positive");
        const cJSON *tot = cJSON_GetObjectItemCaseSensitive(sum, "total_reviews");
        const cJSON *desc =
            cJSON_GetObjectItemCaseSensitive(sum, "review_score_desc");
        int64_t p = cJSON_IsNumber(pos) ? (int64_t)pos->valuedouble : 0;
        int64_t t = cJSON_IsNumber(tot) ? (int64_t)tot->valuedouble : 0;

        if (t > 0) {
            *pct = (int)((p * 100) / t);
            *count = t > 2000000000 ? 2000000000 : (int)t;
        } else {
            *pct = 0;
            *count = 0;
        }
        if (cJSON_IsString(desc) && desc->valuestring && label && labelsz) {
            snprintf(label, labelsz, "%s", desc->valuestring);
        }
    }
    cJSON_Delete(root);
    return 0;
}

static int
save_cover(vapord *app, const char *game_id, const char *version,
           const char *header_url, int appid)
{
    char dest[VAPORD_PATH_MAX];
    char capsule[256];
    char have[256];

    if (vapord_version_cover(app->db, game_id, version, have, sizeof(have))
        == 0) {
        return 0;
    }
    if (vapord_content_path(&app->cfg, game_id, version, "cover.jpg", dest,
                            sizeof(dest))
        != 0) {
        return -1;
    }

    snprintf(capsule, sizeof(capsule),
             "https://cdn.akamai.steamstatic.com/steam/apps/%d/library_600x900.jpg",
             appid);
    if (http_get_file(capsule, dest, VAPOR_MAX_COVER_BYTES) != 0) {
        if (!header_url || !*header_url
            || http_get_file(header_url, dest, VAPOR_MAX_COVER_BYTES) != 0) {
            return 1;
        }
    }
    if (vapord_version_set_cover(app->db, game_id, version, "cover.jpg") != 0) {
        return -1;
    }
    return 0;
}

int
vapord_meta_enrich(vapord *app, const char *game_id, const char *name,
                   const char *version, int hint_appid, int force)
{
    char    developer[128] = "";
    char    description[VAPOR_DESC_MAX] = "";
    char    have_dev[128] = "";
    char    have_desc[VAPOR_DESC_MAX] = "";
    char    header[1024] = "";
    char    label[48] = "";
    int     appid = 0, stored_appid = 0;
    int     pct = -1, count = -1;
    int64_t fetched = 0;
    int     rc;

    if (!app || !game_id || !name) {
        return 0;
    }
    if (vapord_game_meta_row(app->db, game_id, have_dev, sizeof(have_dev),
                             have_desc, sizeof(have_desc), &stored_appid,
                             &fetched)
        != 0) {
        return 0;
    }
    if (!force && fetched > 0
        && vapor_now_unix() - fetched < META_CACHE_SECS) {
        return 0;
    }

    appid = hint_appid > 0 ? hint_appid : stored_appid;
    if (appid <= 0) {
        rc = search_steam(name, &appid);
        if (rc != 0 || appid <= 0) {
            vapord_game_touch_meta(app->db, game_id);
            if (rc < 0) {
                VLOG_WARN("meta: Steam search failed for \"%s\"", name);
            }
            return 1;
        }
        VLOG_INFO("meta: \"%s\" matched Steam app %d", name, appid);
    }

    rc = fetch_details(appid, developer, sizeof(developer), description,
                       sizeof(description), header, sizeof(header));
    if (rc != 0) {
        VLOG_WARN("meta: Steam details failed for %s (app %d)", game_id, appid);
        vapord_game_touch_meta(app->db, game_id);
        return 1;
    }

    (void)fetch_reviews(appid, &pct, &count, label, sizeof(label));

    if (have_dev[0]) {
        developer[0] = '\0';
    }
    if (have_desc[0]) {
        description[0] = '\0';
    }

    if (vapord_game_set_meta(app->db, game_id, developer, description, appid,
                             pct, count, label)
        != 0) {
        VLOG_WARN("meta: cannot store Steam metadata for %s", game_id);
        return 1;
    }

    if (version && *version) {
        rc = save_cover(app, game_id, version, header, appid);
        if (rc < 0) {
            VLOG_WARN("meta: cannot store cover for %s", game_id);
        } else if (rc == 0) {
            VLOG_INFO("meta: cover saved for %s", game_id);
        }
    }
    if (label[0]) {
        VLOG_INFO("meta: %s  %s (%d%% of %d)", game_id, label, pct < 0 ? 0 : pct,
                  count < 0 ? 0 : count);
    }
    return 1;
}

int
vapord_meta_enrich_all(vapord *app, int force)
{
    sqlite3_stmt *st = NULL;
    int           n = 0;

    if (sqlite3_prepare_v2(app->db, "SELECT id, name FROM games ORDER BY name",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *id   = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        char        latest[VAPOR_VERSION_MAX + 1];

        if (!id || !name) {
            continue;
        }
        latest[0] = '\0';
        (void)vapord_game_latest_version(app->db, id, latest, sizeof(latest));
        if (vapord_meta_enrich(app, id, name, latest, 0, force) == 1) {
            n++;
        }
    }
    sqlite3_finalize(st);
    VLOG_INFO("meta: looked up %d title(s)", n);
    return 0;
}
