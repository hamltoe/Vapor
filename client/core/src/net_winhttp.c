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

#ifndef _CRT_WIDE
#define VAPOR_CRT_WIDE_IMPL(s) L##s
#define _CRT_WIDE(s) VAPOR_CRT_WIDE_IMPL(s)
#endif

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

#if defined(VAPOR_TARGET_XP)
/* XP's WinHttpCrackUrl returns ERROR_WINHTTP_INVALID_URL (12005) for a
 * normal LAN address such as http://192.168.1.107:8777/api/v1/health.
 * Split http://host[:port][/path] here and pass the pieces to WinHttpConnect. */
static int
parse_http_url(const char *url, char *host, size_t hostsz, char *path,
               size_t pathsz, int *port, int *https)
{
    const char *p, *slash, *colon;
    size_t      hostlen, pathlen;
    int         prt;

    if (!url) {
        return -1;
    }
    if (strncmp(url, "https://", 8) == 0) {
        *https = 1;
        p = url + 8;
        prt = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        *https = 0;
        p = url + 7;
        prt = 80;
    } else {
        return -1;
    }
    if (*p == '\0' || *p == '/' || *p == ':' || *p == '@') {
        return -1;
    }

    slash = strchr(p, '/');
    if (!slash) {
        slash = p + strlen(p);
    }
    if (memchr(p, '@', (size_t)(slash - p)) || memchr(p, ' ', (size_t)(slash - p))) {
        return -1;
    }

    colon = NULL;
    for (const char *q = p; q < slash; q++) {
        if (*q == ':') {
            colon = q;
        }
    }
    if (colon) {
        long acc = 0;
        hostlen = (size_t)(colon - p);
        if (colon + 1 == slash) {
            return -1;
        }
        for (const char *q = colon + 1; q < slash; q++) {
            if (*q < '0' || *q > '9') {
                return -1;
            }
            acc = acc * 10 + (*q - '0');
            if (acc > 65535) {
                return -1;
            }
        }
        prt = (int)acc;
    } else {
        hostlen = (size_t)(slash - p);
    }
    if (hostlen == 0 || hostlen >= hostsz) {
        return -1;
    }
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';

    if (*slash == '\0') {
        if (pathsz < 2) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
    } else {
        pathlen = strlen(slash);
        if (pathlen >= pathsz) {
            return -1;
        }
        memcpy(path, slash, pathlen + 1);
    }
    *port = prt;
    return 0;
}
#endif

int
vapor_net_perform(const vapor_net_req *req, vapor_net_res *res)
{
    HINTERNET  session = NULL, connect = NULL, request = NULL;
    wchar_t   *wurl = NULL, *wmethod = NULL;
    wchar_t    host[256], path[2048];
    vapor_buf  mem;
    FILE      *file = NULL;
    DWORD      status = 0, status_sz = sizeof(status);
    DWORD      flags = 0;
    INTERNET_PORT port = 0;
    int        https = 0;
    int        resuming = 0, error_body = 0, ret = -1;
    uint64_t   written = 0, total = 0;

    vapor_buf_init(&mem);

    wmethod = widen(req->method);
    if (!wmethod) {
        snprintf(res->err, sizeof(res->err), "out of memory");
        goto cleanup;
    }

#if defined(VAPOR_TARGET_XP)
    {
        char     host_a[256], path_a[2048];
        wchar_t *wh, *wp;
        int      port_i = 0;

        if (parse_http_url(req->url, host_a, sizeof(host_a), path_a,
                           sizeof(path_a), &port_i, &https)
            != 0) {
            snprintf(res->err, sizeof(res->err), "malformed server URL: %.240s",
                     req->url ? req->url : "");
            goto cleanup;
        }
        wh = widen(host_a);
        wp = widen(path_a);
        if (!wh || !wp
            || wcslen(wh) >= sizeof(host) / sizeof(host[0])
            || wcslen(wp) >= sizeof(path) / sizeof(path[0])) {
            free(wh);
            free(wp);
            snprintf(res->err, sizeof(res->err), "malformed server URL: %.240s",
                     req->url ? req->url : "");
            goto cleanup;
        }
        wcscpy(host, wh);
        wcscpy(path, wp);
        free(wh);
        free(wp);
        port = (INTERNET_PORT)port_i;
    }
#else
    {
        URL_COMPONENTS uc;

        wurl = widen(req->url);
        if (!wurl) {
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
        port = uc.nPort;
        https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    }
#endif

    /* AUTOMATIC_PROXY is Windows 8.1. The XP client is LAN-only, so a
     * configured IE proxy must not sit on the path and reset the socket. */
#if defined(VAPOR_TARGET_XP)
    session = WinHttpOpen(L"vapor-client/" _CRT_WIDE(VAPOR_VERSION_STRING),
                          WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#else
    session = WinHttpOpen(L"vapor-client/" _CRT_WIDE(VAPOR_VERSION_STRING),
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!session) {
        fail(res, "WinHttpOpen failed");
        goto cleanup;
    }

    /* 15s to connect; no total timeout, since a large package legitimately
     * takes a long while. 60s of silence is treated as a dead connection. */
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 0);

    connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        fail(res, "cannot connect to server");
        goto cleanup;
    }

    if (https) {
        flags |= WINHTTP_FLAG_SECURE;
    }
    /* XP's WinHTTP resets an HTTP/1.1 keep-alive exchange (error 12030). */
    request = WinHttpOpenRequest(connect, wmethod, path,
#if defined(VAPOR_TARGET_XP)
                                 L"HTTP/1.0",
#else
                                 NULL,
#endif
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        fail(res, "WinHttpOpenRequest failed");
        goto cleanup;
    }
#if defined(VAPOR_TARGET_XP)
    WinHttpAddRequestHeaders(request, L"Connection: Close", (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);
#endif

#if !defined(VAPOR_TARGET_XP)
    /* WINHTTP_OPTION_REDIRECT_POLICY is Vista. XP follows its own redirect rules. */
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;

        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                         sizeof(policy));
    }
#endif

    WinHttpAddRequestHeaders(request,
                             req->dest_path ? L"Accept: */*"
                                            : L"Accept: application/json",
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (req->body) {
        char     ctype[160];
        wchar_t *wtype;
        snprintf(ctype, sizeof(ctype), "Content-Type: %s",
                 req->content_type ? req->content_type : "application/json");
        wtype = widen(ctype);
        if (wtype) {
            WinHttpAddRequestHeaders(request, wtype, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
            free(wtype);
        }
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
                            req->body ? (DWORD)req->body_len : 0,
                            req->body ? (DWORD)req->body_len : 0, 0)) {
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
