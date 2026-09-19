#if defined(_WIN32)

/* WinHTTP backend. Chosen over libcurl on Windows because winhttp.lib is part
 * of the Windows SDK: the client builds and ships with no third-party runtime,
 * and TLS uses the system certificate store automatically. */

#include "net.h"

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/buf.h"

int
vapor_net_global_init(void)
{
    return 0;
}

void
vapor_net_global_cleanup(void)
{
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

static wchar_t *
widen(const char *s)
{
    int      n;
    wchar_t *w;

    if (!s) {
        return NULL;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static void
fail(vapor_net_res *res, const char *what)
{
    snprintf(res->err, sizeof(res->err), "%s (WinHTTP error %lu)", what,
             GetLastError());
}

int
vapor_net_perform(const vapor_net_req *req, vapor_net_res *res)
{
    HINTERNET  session = NULL, connect = NULL, request = NULL;
    wchar_t   *wurl = NULL, *wmethod = NULL;
    wchar_t    host[256], path[2048];
    URL_COMPONENTS uc;
    vapor_buf  mem;
    FILE      *file = NULL;
    DWORD      status = 0, status_sz = sizeof(status);
    DWORD      flags = 0;
    int        resuming = 0, error_body = 0, ret = -1;
    uint64_t   written = 0, total = 0;

    vapor_buf_init(&mem);

    wurl = widen(req->url);
    wmethod = widen(req->method);
    if (!wurl || !wmethod) {
        snprintf(res->err, sizeof(res->err), "out of memory");
        goto cleanup;
    }

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        fail(res, "malformed server URL");
        goto cleanup;
    }

    session = WinHttpOpen(L"vapor-client/" _CRT_WIDE(VAPOR_VERSION_STRING),
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        fail(res, "WinHttpOpen failed");
        goto cleanup;
    }

    /* 15s to connect; no total timeout, since a large package legitimately
     * takes a long while. 60s of silence is treated as a dead connection. */
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 0);

    connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        fail(res, "cannot connect to server");
        goto cleanup;
    }

    if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
        flags |= WINHTTP_FLAG_SECURE;
    }
    request = WinHttpOpenRequest(connect, wmethod, path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        fail(res, "WinHttpOpenRequest failed");
        goto cleanup;
    }

    WinHttpAddRequestHeaders(request, L"Accept: application/json", (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);
    if (req->body) {
        WinHttpAddRequestHeaders(request, L"Content-Type: application/json",
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (req->bearer) {
        char     hdr[128];
        wchar_t *whdr;
        snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", req->bearer);
        whdr = widen(hdr);
        if (whdr) {
            WinHttpAddRequestHeaders(request, whdr, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
            free(whdr);
        }
    }

    if (req->dest_path) {
        resuming = req->resume_from > 0;
        if (resuming) {
            char     hdr[64];
            wchar_t *whdr;
            snprintf(hdr, sizeof(hdr), "Range: bytes=%llu-",
                     (unsigned long long)req->resume_from);
            whdr = widen(hdr);
            if (whdr) {
                WinHttpAddRequestHeaders(request, whdr, (DWORD)-1,
                                         WINHTTP_ADDREQ_FLAG_ADD);
                free(whdr);
            }
        }
        file = fopen(req->dest_path, resuming ? "ab" : "wb");
        if (!file) {
            snprintf(res->err, sizeof(res->err), "cannot write %s",
                     req->dest_path);
            goto cleanup;
        }
        written = req->resume_from;
    }

    if (req->pinned_pubkey) {
        /* Public-key pinning is not wired up on this backend yet; WinHTTP
         * requires inspecting the cert context in a status callback. The pin is
         * honoured by the libcurl backend, so refuse rather than silently
         * downgrade to plain CA validation. */
        snprintf(res->err, sizeof(res->err),
                 "pinned_pubkey is not supported by the Windows backend yet; "
                 "use a CA-issued certificate");
        goto cleanup;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            req->body ? (LPVOID)req->body
                                      : WINHTTP_NO_REQUEST_DATA,
                            req->body ? (DWORD)strlen(req->body) : 0,
                            req->body ? (DWORD)strlen(req->body) : 0, 0)) {
        fail(res, "cannot reach server");
        goto cleanup;
    }
    if (!WinHttpReceiveResponse(request, NULL)) {
        fail(res, "no response from server");
        goto cleanup;
    }

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE
                                 | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                             WINHTTP_NO_HEADER_INDEX)) {
        fail(res, "cannot read response status");
        goto cleanup;
    }
    res->status = (long)status;

    if (status >= 400) {
        error_body = 1;
    } else if (file && resuming && status == 200) {
        /* Range ignored: restart the file from zero. */
        FILE *re = freopen(req->dest_path, "wb", file);
        if (!re) {
            snprintf(res->err, sizeof(res->err), "cannot rewrite %s",
                     req->dest_path);
            goto cleanup;
        }
        file = re;
        written = 0;
    }

    {
        DWORD len_sz = sizeof(DWORD);
        DWORD content_len = 0;
        if (WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_CONTENT_LENGTH
                                   | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &content_len,
                               &len_sz, WINHTTP_NO_HEADER_INDEX)) {
            total = written + content_len;
        }
    }

    for (;;) {
        DWORD avail = 0, got = 0;
        char  chunk[16384];

        if (!WinHttpQueryDataAvailable(request, &avail)) {
            fail(res, "read failed");
            goto cleanup;
        }
        if (avail == 0) {
            break;
        }
        while (avail > 0) {
            DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
            if (!WinHttpReadData(request, chunk, want, &got) || got == 0) {
                break;
            }
            if (error_body || !file) {
                if (vapor_buf_append(&mem, chunk, got) != 0) {
                    snprintf(res->err, sizeof(res->err), "out of memory");
                    goto cleanup;
                }
            } else {
                if (fwrite(chunk, 1, got, file) != got) {
                    snprintf(res->err, sizeof(res->err), "write failed");
                    goto cleanup;
                }
                written += got;
                if (req->progress
                    && req->progress(req->progress_ud, written, total) != 0) {
                    res->aborted = 1;
                    snprintf(res->err, sizeof(res->err), "cancelled");
                    goto cleanup;
                }
            }
            avail -= got;
        }
    }

    ret = 0;

cleanup:
    if (file) {
        fclose(file);
    }
    res->body_len = mem.len;
    res->body = vapor_buf_release(&mem);

    if (request) { WinHttpCloseHandle(request); }
    if (connect) { WinHttpCloseHandle(connect); }
    if (session) { WinHttpCloseHandle(session); }
    free(wurl);
    free(wmethod);

    /* A 4xx is a real answer, not a transport failure. */
    if (ret != 0 && res->status >= 400 && !res->aborted) {
        ret = 0;
    }
    return ret;
}

#endif /* _WIN32 */
