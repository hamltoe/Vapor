/* Host window for vapord. The HTTP server is already running; this is the
 * garage-monitor front: status, catalog, logs, and a Discover button.
 * Closing the window stops the process. */

#define SDL_MAIN_HANDLED
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include "vapord.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"
#include "nuklear_sdl_gl2.h"

#define HOST_W  760
#define HOST_H  560
#define LOG_N   28
#define LOG_W   280
#define TITLE_N 48
#define TITLE_W 96

typedef struct {
    char               lines[LOG_N][LOG_W];
    int                n;
    int                head;
    pthread_mutex_t    mu;
    vapord            *app;
    volatile int      *stop;
    volatile int       scanning;
    int64_t            users;
    int64_t            games;
    char               titles[TITLE_N][TITLE_W];
    size_t             ntitles;
} host_ui;

static void
log_sink(const char *line, void *ud)
{
    host_ui *ui = ud;

    pthread_mutex_lock(&ui->mu);
    snprintf(ui->lines[ui->head], sizeof(ui->lines[0]), "%s", line ? line : "");
    ui->head = (ui->head + 1) % LOG_N;
    if (ui->n < LOG_N) {
        ui->n++;
    }
    pthread_mutex_unlock(&ui->mu);
}

static void *
scan_main(void *arg)
{
    host_ui *ui = arg;

    vapord_discover(ui->app);
    ui->scanning = 0;
    return NULL;
}

static void
request_scan(host_ui *ui)
{
    pthread_t th;

    if (!ui->app->cfg.library_root[0] || ui->scanning) {
        return;
    }
    ui->scanning = 1;
    if (pthread_create(&th, NULL, scan_main, ui) != 0) {
        ui->scanning = 0;
        return;
    }
    pthread_detach(th);
}

static void
refresh_stats(host_ui *ui)
{
    vapord_user_count(ui->app->db, &ui->users);
    vapord_game_count(ui->app->db, &ui->games);
    vapord_game_titles(ui->app->db, (char *)ui->titles, TITLE_W, TITLE_N,
                       &ui->ntitles);
}

static void
style(struct nk_context *ctx)
{
    struct nk_color t[NK_COLOR_COUNT];
    struct nk_color bg = nk_rgb(22, 26, 32);
    struct nk_color panel = nk_rgb(32, 37, 45);
    struct nk_color accent = nk_rgb(66, 150, 210);

    memset(t, 0, sizeof(t));
    t[NK_COLOR_TEXT] = nk_rgb(222, 227, 234);
    t[NK_COLOR_WINDOW] = bg;
    t[NK_COLOR_HEADER] = panel;
    t[NK_COLOR_BORDER] = nk_rgb(58, 66, 78);
    t[NK_COLOR_BUTTON] = accent;
    t[NK_COLOR_BUTTON_HOVER] = nk_rgb(86, 172, 232);
    t[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(46, 124, 180);
    t[NK_COLOR_TOGGLE] = nk_rgb(41, 47, 57);
    t[NK_COLOR_TOGGLE_HOVER] = nk_rgb(51, 58, 70);
    t[NK_COLOR_TOGGLE_CURSOR] = accent;
    t[NK_COLOR_SELECT] = panel;
    t[NK_COLOR_SELECT_ACTIVE] = accent;
    t[NK_COLOR_SLIDER] = nk_rgb(41, 47, 57);
    t[NK_COLOR_SLIDER_CURSOR] = accent;
    t[NK_COLOR_SLIDER_CURSOR_HOVER] = t[NK_COLOR_BUTTON_HOVER];
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = t[NK_COLOR_BUTTON_ACTIVE];
    t[NK_COLOR_PROPERTY] = nk_rgb(26, 30, 37);
    t[NK_COLOR_EDIT] = nk_rgb(26, 30, 37);
    t[NK_COLOR_EDIT_CURSOR] = t[NK_COLOR_TEXT];
    t[NK_COLOR_COMBO] = panel;
    t[NK_COLOR_CHART] = panel;
    t[NK_COLOR_CHART_COLOR] = accent;
    t[NK_COLOR_SCROLLBAR] = nk_rgb(26, 30, 37);
    t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(58, 66, 78);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(78, 88, 102);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = accent;
    t[NK_COLOR_TAB_HEADER] = panel;
    nk_style_from_table(ctx, t);
}

int
vapord_gui_run(vapord *app, volatile int *stop)
{
    host_ui            ui;
    SDL_Window        *win;
    SDL_GLContext      gl;
    struct nk_context *ctx;
    struct nk_font_atlas *atlas;
    int64_t            last_discover, last_stats, last_prune;

    memset(&ui, 0, sizeof(ui));
    ui.app = app;
    ui.stop = stop;
    pthread_mutex_init(&ui.mu, NULL);
    vapord_set_log_sink(log_sink, &ui);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        vapord_set_log_sink(NULL, NULL);
        pthread_mutex_destroy(&ui.mu);
        return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    win = SDL_CreateWindow("Vapor Server", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, HOST_W, HOST_H,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                               | SDL_WINDOW_RESIZABLE);
    if (!win) {
        SDL_Quit();
        vapord_set_log_sink(NULL, NULL);
        pthread_mutex_destroy(&ui.mu);
        return -1;
    }
    gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win);
        SDL_Quit();
        vapord_set_log_sink(NULL, NULL);
        pthread_mutex_destroy(&ui.mu);
        return -1;
    }
    SDL_GL_SetSwapInterval(1);

    ctx = nk_sdl_init(win);
    nk_sdl_font_stash_begin(&atlas);
    {
        struct nk_font *font = nk_font_atlas_add_default(atlas, 16.0f, NULL);
        nk_sdl_font_stash_end();
        if (font) {
            nk_style_set_font(ctx, &font->handle);
        }
    }
    style(ctx);

    refresh_stats(&ui);
    last_stats = vapor_now_unix();
    last_discover = last_stats;
    last_prune = last_stats;
    request_scan(&ui);

    while (!*stop) {
        SDL_Event evt;
        int       w, h;
        int64_t   now;

        nk_input_begin(ctx);
        if (SDL_WaitEventTimeout(&evt, ui.scanning ? 16 : 200)) {
            do {
                if (evt.type == SDL_QUIT) {
                    *stop = 1;
                }
                nk_sdl_handle_event(&evt);
            } while (SDL_PollEvent(&evt));
        }
        nk_sdl_handle_grab();
        nk_input_end(ctx);

        now = vapor_now_unix();
        if (now - last_stats >= 1) {
            refresh_stats(&ui);
            last_stats = now;
        }
        if (app->cfg.library_root[0] && app->cfg.discover_interval > 0
            && now - last_discover >= app->cfg.discover_interval) {
            request_scan(&ui);
            last_discover = now;
        }
        if (now - last_prune >= 3600) {
            vapord_token_prune(app->db, now);
            last_prune = now;
        }

        SDL_GetWindowSize(win, &w, &h);
        if (nk_begin(ctx, "Vapor Server", nk_rect(0, 0, (float)w, (float)h),
                     NK_WINDOW_NO_SCROLLBAR)) {
            char line[VAPORD_PATH_MAX + 32];
            int  i;

            nk_layout_row_dynamic(ctx, 28, 1);
            nk_label(ctx, "Vapor host", NK_TEXT_LEFT);

            nk_layout_row_dynamic(ctx, 22, 1);
            snprintf(line, sizeof(line), "listening  %s:%d",
                     app->cfg.bind_addr, app->cfg.port);
            nk_label(ctx, line, NK_TEXT_LEFT);
            snprintf(line, sizeof(line), "library    %s",
                     app->cfg.library_root[0] ? app->cfg.library_root
                                              : "(disabled)");
            nk_label(ctx, line, NK_TEXT_LEFT);
            snprintf(line, sizeof(line), "content    %s", app->cfg.content_root);
            nk_label(ctx, line, NK_TEXT_LEFT);
            snprintf(line, sizeof(line), "accounts   %lld    games   %lld%s",
                     (long long)ui.users, (long long)ui.games,
                     ui.scanning ? "    scanning..." : "");
            nk_label(ctx, line, NK_TEXT_LEFT);

            nk_layout_row_dynamic(ctx, 34, 2);
            if (nk_button_label(ctx, ui.scanning ? "Scanning..." : "Discover now")) {
                request_scan(&ui);
                last_discover = now;
            }
            if (nk_button_label(ctx, "Stop server")) {
                *stop = 1;
            }

            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "Games", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 140, 1);
            if (nk_group_begin(ctx, "games", NK_WINDOW_BORDER)) {
                nk_layout_row_dynamic(ctx, 20, 1);
                if (ui.ntitles == 0) {
                    nk_label(ctx, "Drop a folder into library_root.",
                             NK_TEXT_LEFT);
                }
                for (i = 0; i < (int)ui.ntitles; i++) {
                    nk_label(ctx, ui.titles[i], NK_TEXT_LEFT);
                }
                nk_group_end(ctx);
            }

            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "Log", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 160, 1);
            if (nk_group_begin(ctx, "log", NK_WINDOW_BORDER)) {
                nk_layout_row_dynamic(ctx, 18, 1);
                pthread_mutex_lock(&ui.mu);
                for (i = 0; i < ui.n; i++) {
                    int idx = (ui.head - ui.n + i + LOG_N) % LOG_N;
                    nk_label(ctx, ui.lines[idx], NK_TEXT_LEFT);
                }
                pthread_mutex_unlock(&ui.mu);
                nk_group_end(ctx);
            }
        }
        nk_end(ctx);

        glViewport(0, 0, w, h);
        glClearColor(22 / 255.0f, 26 / 255.0f, 32 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_GL_SwapWindow(win);
    }

    while (ui.scanning) {
        SDL_Delay(50);
    }
    vapord_set_log_sink(NULL, NULL);
    nk_sdl_shutdown();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    pthread_mutex_destroy(&ui.mu);
    return 0;
}
