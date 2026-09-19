/* Shared state for vapor-gui.
 *
 * Nuklear is a single-header library, so the NK_INCLUDE_* switches have to be
 * identical in every translation unit that sees it. Funnelling the include
 * through this header is what guarantees that; only main.c additionally defines
 * NK_IMPLEMENTATION. */

#ifndef VAPOR_GUI_H
#define VAPOR_GUI_H

/* Before SDL.h, or SDL renames main to SDL_main and expects SDL2main to supply
 * the real entry point. We keep a plain main so vapor-gui stays an ordinary
 * console program and libvapor's diagnostics remain visible. */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "vapor/client.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"

#define VAPOR_GUI_TITLE   "Vapor"
#define VAPOR_GUI_WIDTH   1100
#define VAPOR_GUI_HEIGHT  700

typedef enum {
    SCREEN_LOGIN,
    SCREEN_LIBRARY
} vapor_screen;

/* Anything that talks to the network or the disk runs on a worker thread, so
 * the window keeps redrawing and the progress bar actually moves. Exactly one
 * job runs at a time; while `active` is set the UI thread must not touch the
 * vapor_client, because the worker owns it for the duration. */
typedef enum {
    JOB_NONE,
    JOB_LOGIN,
    JOB_REGISTER,
    JOB_REFRESH,
    JOB_INSTALL,
    JOB_UNINSTALL,
    JOB_VERIFY,
    JOB_LAUNCH
} vapor_job_kind;

typedef struct {
    SDL_mutex     *lock;
    SDL_Thread    *thread;
    vapor_job_kind kind;
    int            active;      /* a thread is running right now */
    int            done;        /* finished and waiting to be reaped */
    int            failed;
    int            cancel;      /* set by the UI, polled by the transfer */

    char     game_id[VAPOR_ID_MAX + 1];
    char     version[VAPOR_VERSION_MAX + 1];
    char     status[256];       /* what the job is doing, shown in the UI */
    char     message[512];      /* the outcome, shown when it finishes */
    uint64_t done_bytes;
    uint64_t total_bytes;
} vapor_job;

/* Cover art, one per catalog row. The worker only ever fills in `path`; the
 * texture is created on the UI thread, because that is the thread holding the
 * OpenGL context. */
typedef struct {
    char     path[VAPOR_PATH_MAX];   /* empty when the game has no cover */
    unsigned tex;                    /* GL texture name, 0 until uploaded */
    int      w, h;
    int      failed;                 /* decode failed once; do not retry */
} vapor_cover;

typedef struct {
    vapor_client *vc;
    vapor_screen  screen;
    vapor_job     job;

    /* Catalog merged with local install state, refreshed after every job that
     * could have changed it. */
    vapor_catalog_entry *games;
    vapor_cover         *covers;   /* ngames entries, parallel to games */
    size_t               ngames;
    int                  catalog_loaded;

    /* A refresh builds its results here and vapor_gui_job_poll swaps them in,
     * so the draw loop never sees the array being replaced underneath it. */
    vapor_catalog_entry *pending_games;
    vapor_cover         *pending_covers;
    size_t               pending_ngames;

    vapor_account account;
    char          server_info[128];

    /* Login form. Nuklear edits fixed buffers in place and tracks the length
     * separately, so each field needs its own length. */
    char username[VAPOR_USERNAME_MAX + 1];
    int  username_len;
    char password[VAPOR_PASSWORD_MAX + 1];
    int  password_len;
    char server_url[512];
    int  server_url_len;
    char library_dir[VAPOR_PATH_MAX];
    int  library_dir_len;
    char notice[512];           /* transient banner under the header */
    int  notice_is_error;

    char  search[64];
    int   search_len;
    int   show_settings;
    char *selected;             /* id of the expanded card, or NULL */
    int   quit;
} vapor_app;

/* ------------------------------------------------------------------- worker */
void vapor_gui_job_init(vapor_app *app);
void vapor_gui_job_shutdown(vapor_app *app);
/* Non-zero if a job is running; the UI disables actions while it is. */
int  vapor_gui_job_busy(vapor_app *app);
/* Reaps a finished job: applies its outcome to the app and clears it. */
void vapor_gui_job_poll(vapor_app *app);
/* Snapshot of the progress fields, taken under the lock. */
void vapor_gui_job_progress(vapor_app *app, char *status, size_t statussz,
                            uint64_t *done, uint64_t *total,
                            vapor_job_kind *kind);

int  vapor_gui_start_login(vapor_app *app, int registering);
int  vapor_gui_start_refresh(vapor_app *app);
/* `version` may be NULL or "latest". */
int  vapor_gui_start_install(vapor_app *app, const char *game_id,
                             const char *version);
int  vapor_gui_start_uninstall(vapor_app *app, const char *game_id);
int  vapor_gui_start_verify(vapor_app *app, const char *game_id);
int  vapor_gui_start_launch(vapor_app *app, const char *game_id);

void vapor_gui_notice(vapor_app *app, int is_error, const char *fmt, ...);

/* ------------------------------------------------------------------- covers */
/* All three touch OpenGL, so they are UI-thread only. */
/* Decodes and uploads on first use. Returns 0 when there is nothing to draw. */
int  vapor_gui_cover_image(vapor_app *app, size_t index, struct nk_image *out);
void vapor_gui_covers_release(vapor_app *app);
/* Replaces games+covers with whatever the last refresh produced. */
void vapor_gui_covers_adopt_pending(vapor_app *app);

/* ------------------------------------------------------------------ screens */
void vapor_gui_login_screen(struct nk_context *ctx, vapor_app *app, int w, int h);
void vapor_gui_library_screen(struct nk_context *ctx, vapor_app *app, int w,
                              int h);
/* Theme, shared by both screens. */
void vapor_gui_style(struct nk_context *ctx);

#endif /* VAPOR_GUI_H */
