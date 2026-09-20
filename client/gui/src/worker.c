/* Background jobs for the GUI.
 *
 * Every libvapor call that can block - anything doing network or disk work -
 * runs here on an SDL thread instead of inside the draw loop. The UI thread and
 * the worker never use the vapor_client at the same time: starting a job hands
 * ownership to the worker, and vapor_gui_job_poll takes it back once the thread
 * has been joined. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "vapor/util.h"

static void
set_status(vapor_app *app, const char *fmt, ...)
{
    va_list ap;

    SDL_LockMutex(app->job.lock);
    va_start(ap, fmt);
    vsnprintf(app->job.status, sizeof(app->job.status), fmt, ap);
    va_end(ap);
    SDL_UnlockMutex(app->job.lock);
}

void
vapor_gui_notice(vapor_app *app, int is_error, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(app->notice, sizeof(app->notice), fmt, ap);
    va_end(ap);
    app->notice_is_error = is_error;
}

/* ------------------------------------------------------------- job lifecycle */

void
vapor_gui_job_init(vapor_app *app)
{
    memset(&app->job, 0, sizeof(app->job));
    app->job.lock = SDL_CreateMutex();
}

void
vapor_gui_job_shutdown(vapor_app *app)
{
    if (app->job.thread) {
        /* Ask the transfer to stop, then wait: detaching would leave the worker
         * writing into state we are about to free. */
        SDL_LockMutex(app->job.lock);
        app->job.cancel = 1;
        SDL_UnlockMutex(app->job.lock);
        SDL_WaitThread(app->job.thread, NULL);
        app->job.thread = NULL;
    }
    if (app->job.lock) {
        SDL_DestroyMutex(app->job.lock);
        app->job.lock = NULL;
    }
}

int
vapor_gui_job_busy(vapor_app *app)
{
    int busy;

    SDL_LockMutex(app->job.lock);
    busy = app->job.active;
    SDL_UnlockMutex(app->job.lock);
    return busy;
}

void
vapor_gui_job_progress(vapor_app *app, char *status, size_t statussz,
                       uint64_t *done, uint64_t *total, vapor_job_kind *kind)
{
    SDL_LockMutex(app->job.lock);
    snprintf(status, statussz, "%s", app->job.status);
    *done = app->job.done_bytes;
    *total = app->job.total_bytes;
    *kind = app->job.kind;
    SDL_UnlockMutex(app->job.lock);
}

/* Progress callback handed to libvapor. Returning non-zero aborts the
 * transfer, which is how the Cancel button takes effect. */
static int
on_progress(void *ud, uint64_t done, uint64_t total)
{
    vapor_app *app = (vapor_app *)ud;
    int        cancel;

    SDL_LockMutex(app->job.lock);
    app->job.done_bytes = done;
    app->job.total_bytes = total;
    /* An install runs manifest -> download -> verify -> extract behind this one
     * callback. Inferring the phase from the byte counts keeps the label honest
     * instead of leaving it on "fetching the manifest" for the whole job. */
    if (total > 0 && done >= total) {
        snprintf(app->job.status, sizeof(app->job.status),
                 "verifying and extracting %s...", app->job.game_id);
    } else {
        snprintf(app->job.status, sizeof(app->job.status), "downloading %s...",
                 app->job.game_id);
    }
    cancel = app->job.cancel;
    SDL_UnlockMutex(app->job.lock);
    return cancel;
}

static void
finish(vapor_app *app, int failed, const char *fmt, ...)
{
    va_list ap;

    SDL_LockMutex(app->job.lock);
    va_start(ap, fmt);
    vsnprintf(app->job.message, sizeof(app->job.message), fmt, ap);
    va_end(ap);
    app->job.failed = failed;
    app->job.done = 1;
    app->job.active = 0;
    SDL_UnlockMutex(app->job.lock);
}

/* --------------------------------------------------------------- the worker */

static int SDLCALL
job_main(void *ud)
{
    vapor_app     *app = (vapor_app *)ud;
    vapor_client  *vc = app->vc;
    vapor_job_kind kind;
    char           game_id[VAPOR_ID_MAX + 1];
    char           version[VAPOR_VERSION_MAX + 1];
    int            rating_score = 0;

    /* Copied out once so the rest of the function needs no lock for them. */
    SDL_LockMutex(app->job.lock);
    kind = app->job.kind;
    snprintf(game_id, sizeof(game_id), "%s", app->job.game_id);
    snprintf(version, sizeof(version), "%s", app->job.version);
    rating_score = app->job.rating_score;
    SDL_UnlockMutex(app->job.lock);

    switch (kind) {
    case JOB_LOGIN:
    case JOB_REGISTER: {
        int rc;

        set_status(app, "connecting to the server...");
        if (kind == JOB_REGISTER) {
            vapor_account acct;
            set_status(app, "creating the account...");
            rc = vapor_auth_register(vc, app->username, app->password, &acct);
            if (rc != 0) {
                finish(app, 1, "%s", vc->err);
                break;
            }
            /* Registering does not return a session, so sign in straight after
             * to save the user doing it twice. */
            set_status(app, "signing in...");
        } else {
            set_status(app, "signing in...");
        }
        rc = vapor_auth_login(vc, app->username, app->password);
        if (rc != 0) {
            finish(app, 1, "%s", vc->err);
            break;
        }
        if (vapor_auth_whoami(vc, &app->account) != 0) {
            /* The token is good even if this extra call failed. */
            snprintf(app->account.username, sizeof(app->account.username), "%s",
                     vc->cfg.username);
        }
        finish(app, 0, "signed in as %s", vc->cfg.username);
        break;
    }

    case JOB_REFRESH: {
        vapor_catalog_entry *rows = NULL;
        vapor_cover         *covers = NULL;
        size_t               n = 0, i;

        set_status(app, "loading the catalog...");
        if (vapor_catalog_fetch(vc, &rows, &n) != 0) {
            finish(app, 1, "%s", vc->err);
            break;
        }
        if (n > 0) {
            covers = (vapor_cover *)calloc(n, sizeof(*covers));
            if (!covers) {
                free(rows);
                finish(app, 1, "out of memory");
                break;
            }
            for (i = 0; i < n; i++) {
                if (!rows[i].has_cover || !rows[i].latest_version[0]) {
                    continue;
                }
                /* A missing or failed cover is not a catalog failure. */
                (void)vapor_fetch_cover(vc, rows[i].id, rows[i].latest_version,
                                        covers[i].path, sizeof(covers[i].path));
            }
        }
        /* Stash for the UI thread. The live games array stays untouched so
         * the draw loop can keep reading it until the join. */
        free(app->pending_games);
        free(app->pending_covers);
        app->pending_games = rows;
        app->pending_covers = covers;
        app->pending_ngames = n;
        finish(app, 0, "%s", "");
        break;
    }

    case JOB_INSTALL: {
        vapor_install_opts opts;
        uint64_t           total;
        char               size[32];

        memset(&opts, 0, sizeof(opts));
        /* The UI only ever offers Install or Update, and both should go ahead
         * even when a record already exists. */
        opts.force = 1;
        set_status(app, "fetching the manifest for %s...", game_id);
        if (vapor_install_game(vc, game_id, version[0] ? version : "latest",
                               &opts, on_progress, app)
            != 0) {
            int cancelled;
            SDL_LockMutex(app->job.lock);
            cancelled = app->job.cancel;
            SDL_UnlockMutex(app->job.lock);
            finish(app, cancelled ? 0 : 1, "%s",
                   cancelled ? "download cancelled" : vc->err);
            break;
        }
        SDL_LockMutex(app->job.lock);
        total = app->job.total_bytes;
        SDL_UnlockMutex(app->job.lock);
        vapor_format_bytes(total, size, sizeof(size));
        finish(app, 0, "installed %s (%s)", game_id, size);
        break;
    }

    case JOB_UNINSTALL:
        set_status(app, "removing %s...", game_id);
        if (vapor_uninstall_game(vc, game_id) != 0) {
            finish(app, 1, "%s", vc->err);
            break;
        }
        finish(app, 0, "removed %s", game_id);
        break;

    case JOB_VERIFY: {
        int rc;

        set_status(app, "verifying %s...", game_id);
        rc = vapor_verify_install(vc, game_id);
        if (rc < 0) {
            finish(app, 1, "%s", vc->err);
        } else if (rc > 0) {
            finish(app, 1, "%s has problems; reinstall it", game_id);
        } else {
            finish(app, 0, "%s matches its manifest", game_id);
        }
        break;
    }

    case JOB_LAUNCH: {
        int exit_code = 0;

        set_status(app, "%s is running...", game_id);
        if (vapor_launch_game(vc, game_id, &exit_code) != 0) {
            finish(app, 1, "%s", vc->err);
            break;
        }
        if (exit_code != 0) {
            finish(app, 1, "%s exited with code %d", game_id, exit_code);
        } else {
            finish(app, 0, "%s exited normally", game_id);
        }
        break;
    }

    case JOB_RATE: {
        vapor_rating r;

        set_status(app, "saving your rating...");
        if (vapor_game_rate(vc, game_id, rating_score, &r) != 0) {
            finish(app, 1, "%s", vc->err);
            break;
        }
        SDL_LockMutex(app->job.lock);
        app->job.rating_avg = r.rating_avg;
        app->job.rating_votes = r.rating_votes;
        app->job.rating_score = r.my_rating;
        SDL_UnlockMutex(app->job.lock);
        finish(app, 0, "rated %s %d / 5", game_id, r.my_rating);
        break;
    }

    case JOB_NONE:
    default:
        finish(app, 1, "nothing to do");
        break;
    }

    return 0;
}

/* Common setup. Refuses to start a second job, which is what keeps the
 * single-owner rule on vapor_client true. */
static int
start(vapor_app *app, vapor_job_kind kind, const char *game_id,
      const char *version, int rating_score)
{
    if (vapor_gui_job_busy(app) || app->job.done) {
        return -1;
    }
    if (app->job.thread) {
        SDL_WaitThread(app->job.thread, NULL);
        app->job.thread = NULL;
    }

    SDL_LockMutex(app->job.lock);
    app->job.kind = kind;
    app->job.active = 1;
    app->job.done = 0;
    app->job.failed = 0;
    app->job.cancel = 0;
    app->job.done_bytes = 0;
    app->job.total_bytes = 0;
    app->job.status[0] = '\0';
    app->job.message[0] = '\0';
    snprintf(app->job.game_id, sizeof(app->job.game_id), "%s",
             game_id ? game_id : "");
    snprintf(app->job.version, sizeof(app->job.version), "%s",
             version ? version : "");
    app->job.rating_score = rating_score;
    app->job.rating_avg = 0;
    app->job.rating_votes = 0;
    SDL_UnlockMutex(app->job.lock);

    app->job.thread = SDL_CreateThread(job_main, "vapor-job", app);
    if (!app->job.thread) {
        SDL_LockMutex(app->job.lock);
        app->job.active = 0;
        SDL_UnlockMutex(app->job.lock);
        vapor_gui_notice(app, 1, "could not start a worker thread: %s",
                         SDL_GetError());
        return -1;
    }
    return 0;
}

int
vapor_gui_start_login(vapor_app *app, int registering)
{
    /* Nuklear leaves the buffers unterminated at the tracked length. */
    app->username[app->username_len] = '\0';
    app->password[app->password_len] = '\0';
    app->server_url[app->server_url_len] = '\0';

    if (vapor_client_set_server_url(app->vc, app->server_url) != 0) {
        vapor_gui_notice(app, 1, "%s", app->vc->err);
        return -1;
    }
    snprintf(app->server_url, sizeof(app->server_url), "%s",
             app->vc->cfg.server_url);
    app->server_url_len = (int)strlen(app->server_url);
    (void)vapor_client_save_config(app->vc);

    return start(app, registering ? JOB_REGISTER : JOB_LOGIN, NULL, NULL, 0);
}

int
vapor_gui_start_refresh(vapor_app *app)
{
    return start(app, JOB_REFRESH, NULL, NULL, 0);
}

int
vapor_gui_start_install(vapor_app *app, const char *game_id, const char *version)
{
    return start(app, JOB_INSTALL, game_id, version, 0);
}

int
vapor_gui_start_uninstall(vapor_app *app, const char *game_id)
{
    return start(app, JOB_UNINSTALL, game_id, NULL, 0);
}

int
vapor_gui_start_verify(vapor_app *app, const char *game_id)
{
    return start(app, JOB_VERIFY, game_id, NULL, 0);
}

int
vapor_gui_start_launch(vapor_app *app, const char *game_id)
{
    return start(app, JOB_LAUNCH, game_id, NULL, 0);
}

int
vapor_gui_start_rate(vapor_app *app, const char *game_id, int score)
{
    return start(app, JOB_RATE, game_id, NULL, score);
}

void
vapor_gui_job_poll(vapor_app *app)
{
    vapor_job_kind kind;
    int            failed;
    char           message[512];

    SDL_LockMutex(app->job.lock);
    if (!app->job.done) {
        SDL_UnlockMutex(app->job.lock);
        return;
    }
    kind = app->job.kind;
    failed = app->job.failed;
    snprintf(message, sizeof(message), "%s", app->job.message);
    {
        char   gid[VAPOR_ID_MAX + 1];
        int    score = app->job.rating_score;
        double avg = app->job.rating_avg;
        int    votes = app->job.rating_votes;

        snprintf(gid, sizeof(gid), "%s", app->job.game_id);
        app->job.done = 0;
        app->job.kind = JOB_NONE;
        SDL_UnlockMutex(app->job.lock);

        /* Joining here is what transfers the client back to this thread. */
        if (app->job.thread) {
            SDL_WaitThread(app->job.thread, NULL);
            app->job.thread = NULL;
        }

        if (message[0]) {
            vapor_gui_notice(app, failed, "%s", message);
        }

        if (failed) {
            return;
        }

        if (kind == JOB_RATE) {
            size_t i;
            for (i = 0; i < app->ngames; i++) {
                if (strcmp(app->games[i].id, gid) == 0) {
                    app->games[i].my_rating = score;
                    app->games[i].rating_avg = avg;
                    app->games[i].rating_votes = votes;
                    break;
                }
            }
            return;
        }
    }

    switch (kind) {
    case JOB_LOGIN:
    case JOB_REGISTER:
        app->screen = SCREEN_LIBRARY;
        vapor_secure_zero(app->password, sizeof(app->password));
        app->password_len = 0;
        vapor_gui_start_refresh(app);
        break;
    case JOB_REFRESH:
        vapor_gui_covers_adopt_pending(app);
        break;
    case JOB_INSTALL:
    case JOB_UNINSTALL:
    case JOB_LAUNCH:
        /* All three change what the library rows should say. */
        vapor_gui_start_refresh(app);
        break;
    default:
        break;
    }
}
