#ifndef VAPOR_NET_H
#define VAPOR_NET_H

#include <stddef.h>
#include <stdint.h>

#include "vapor/client.h"

/* Internal transport seam. Two implementations:
 *
 *   net_curl.c    - libcurl, used on Linux and any POSIX host
 *   net_winhttp.c - WinHTTP, used on Windows
 *
 * WinHTTP ships with the OS, so the Windows client needs no third-party
 * dependency at all; libcurl is the natural choice everywhere else because
 * every distribution already has it. Both honour the same request struct, so
 * nothing above this header is platform-aware. */

typedef struct {
    const char *method;
    const char *url;
    const char *body;           /* request body, or NULL */
    const char *bearer;         /* token for the Authorization header, or NULL */
    const char *pinned_pubkey;  /* "sha256//..." pin, or NULL */

    /* Download mode: when dest_path is set the response body streams to that
     * file instead of memory, starting at resume_from. */
    const char       *dest_path;
    uint64_t          resume_from;
    vapor_progress_fn progress;
    void             *progress_ud;
} vapor_net_req;

typedef struct {
    long   status;
    char  *body;      /* memory mode, or an error body captured in download mode */
    size_t body_len;
    int    aborted;   /* the progress callback asked to stop */
    char   err[512];
} vapor_net_res;

int  vapor_net_perform(const vapor_net_req *req, vapor_net_res *res);
void vapor_net_res_free(vapor_net_res *res);

/* Process-wide init/teardown, called from vapor_client_open/close paths. */
int  vapor_net_global_init(void);
void vapor_net_global_cleanup(void);

#endif /* VAPOR_NET_H */
