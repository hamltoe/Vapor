#if !defined(_WIN32)

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "vapor/buf.h"

int
vapor_net_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void
vapor_net_global_cleanup(void)
{
    curl_global_cleanup();
}

void
vapor_net_res_free(vapor_net_res *res)
{
    if (res) {
        free(res->body);
        res->body = NULL;
        res->body_len = 0;
    }
}

typedef struct {
    CURL       *curl;
    vapor_buf   mem;        /* memory mode, or a captured error body */
    FILE       *file;       /* download mode */
    const char *path;
    int         resuming;
    int         status_checked;
    int         error_body; /* status >= 400: keep the body in memory */
    long        status;
} sink;

static size_t
on_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    sink  *s = (sink *)ud;
    size_t n = size * nmemb;

    if (!s->status_checked) {
        s->status_checked = 1;
        curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &s->status);

        if (s->status >= 400) {
            /* Keep the JSON error in memory instead of writing it into what is
             * supposed to be a game archive. */
            s->error_body = 1;
        } else if (s->file && s->resuming && s->status == 200) {
            /* The server ignored our Range header and is sending the whole
             * file, so discard what we had and start over. */
            FILE *re = freopen(s->path, "wb", s->file);
            if (!re) {
                return 0;
            }
            s->file = re;
        }
    }

    if (s->error_body || !s->file) {
        return vapor_buf_append(&s->mem, ptr, n) == 0 ? n : 0;
    }
    return fwrite(ptr, 1, n, s->file);
}

typedef struct {
    vapor_progress_fn cb;
    void             *ud;
    uint64_t          base;   /* bytes already on disk before this transfer */
    int               aborted;
} progress_ctx;

static int
on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
            curl_off_t ulnow)
{
    progress_ctx *p = (progress_ctx *)ud;

    (void)ultotal;
    (void)ulnow;
    if (!p->cb) {
        return 0;
    }
    /* Report absolute file progress, not just this connection's slice, so a
     * resumed download shows a sensible percentage. */
    if (p->cb(p->ud, p->base + (uint64_t)dlnow,
              dltotal > 0 ? p->base + (uint64_t)dltotal : 0)
        != 0) {
        p->aborted = 1;
        return 1;
    }
    return 0;
}

int
vapor_net_perform(const vapor_net_req *req, vapor_net_res *res)
{
    CURL              *curl;
    CURLcode           rc;
    struct curl_slist *headers = NULL;
    sink               s;
    progress_ctx       prog;
    char               auth_hdr[128];
    char               range_hdr[64];
    int                ret = -1;

    memset(&s, 0, sizeof(s));
    memset(&prog, 0, sizeof(prog));
    vapor_buf_init(&s.mem);

    curl = curl_easy_init();
    if (!curl) {
        snprintf(res->err, sizeof(res->err), "curl_easy_init failed");
        return -1;
    }
    s.curl = curl;
    s.path = req->dest_path;

    if (req->dest_path) {
        s.resuming = req->resume_from > 0;
        s.file = fopen(req->dest_path, s.resuming ? "ab" : "wb");
        if (!s.file) {
            snprintf(res->err, sizeof(res->err), "cannot write %s",
                     req->dest_path);
            curl_easy_cleanup(curl);
            return -1;
        }
        if (s.resuming) {
            snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%llu-",
                     (unsigned long long)req->resume_from);
            headers = curl_slist_append(headers, range_hdr);
        }
        prog.base = req->resume_from;
    }

    headers = curl_slist_append(headers, "Accept: application/json");
    if (req->body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    if (req->bearer) {
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s",
                 req->bearer);
        headers = curl_slist_append(headers, auth_hdr);
    }

    prog.cb = req->progress;
    prog.ud = req->progress_ud;

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, VAPOR_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    /* No CURLOPT_TIMEOUT: a multi-gigabyte download is allowed to take as long
     * as it takes, but a stalled connection is cut off below. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);

    if (req->pinned_pubkey) {
        curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, req->pinned_pubkey);
    }

    if (strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)(req->body ? strlen(req->body) : 0));
    } else if (strcmp(req->method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             (long)strlen(req->body));
        }
    }

    rc = curl_easy_perform(curl);

    if (s.file) {
        fclose(s.file);
        s.file = NULL;
    }

    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res->status);
        ret = 0;
    } else if (prog.aborted) {
        res->aborted = 1;
        snprintf(res->err, sizeof(res->err), "cancelled");
    } else {
        /* A 4xx in download mode trips CURLE_WRITE_ERROR only if we refused the
         * body; the status is still worth reporting. */
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res->status);
        if (res->status >= 400) {
            ret = 0;
        } else {
            snprintf(res->err, sizeof(res->err), "%s", curl_easy_strerror(rc));
        }
    }

    res->body_len = s.mem.len;
    res->body = vapor_buf_release(&s.mem);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

#endif /* !_WIN32 */
