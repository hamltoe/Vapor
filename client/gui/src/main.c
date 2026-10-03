/* vapor-gui: a Nuklear + SDL2 shell over libvapor.
 *
 * It adds no client logic of its own. Everything it does - sign in, list the
 * catalog, install, launch - is the same libvapor call the CLI makes, just
 * moved onto a worker thread so the window stays responsive. */

#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#if defined(VAPOR_TARGET_XP)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <SDL_opengl.h>

#include "nuklear_sdl_gl2.h"

#include "vapor/util.h"

/* Nuklear redraws on demand rather than continuously; this is the ceiling when
 * something is animating, such as a download progress bar. */
#define FRAME_MS 16

static void
seed_form(vapor_app *app)
{
    snprintf(app->server_url, sizeof(app->server_url), "%s",
             app->vc->cfg.server_url);
    app->server_url_len = (int)strlen(app->server_url);

    snprintf(app->library_dir, sizeof(app->library_dir), "%s",
             app->vc->cfg.library_dir);
    app->library_dir_len = (int)strlen(app->library_dir);

    snprintf(app->username, sizeof(app->username), "%s", app->vc->cfg.username);
    app->username_len = (int)strlen(app->username);
}

static void
usage(void)
{
    printf("vapor-gui %s - Vapor game library\n\n", VAPOR_VERSION_STRING);
    printf("usage: vapor-gui [--install GAME]\n\n");
    printf("  --install GAME   start installing GAME as soon as the window is\n");
    printf("                   up, for shortcuts and for testing\n");
    printf("  -h, --help       this message\n");
}

/* Depth 24 and stencil 8 are what Nuklear's GL2 path prefers. The Microsoft
 * software rasterizer on XP often cannot provide them, so that build retries
 * with a plainer pixel format. Attributes have to be set before the window. */
static SDL_Window *
open_gl_window(SDL_GLContext *out_gl)
{
    static const int depths[] = {24, 16, 0};
    static const int stencils[] = {8, 0, 0};
    int              i;
    int              tries = 1;

#if defined(VAPOR_TARGET_XP)
    tries = 3;
#endif
    *out_gl = NULL;
    for (i = 0; i < tries; i++) {
        SDL_Window   *win;
        SDL_GLContext gl;

        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depths[i]);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencils[i]);
        win = SDL_CreateWindow(VAPOR_GUI_TITLE, SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, VAPOR_GUI_WIDTH,
                               VAPOR_GUI_HEIGHT,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                                   | SDL_WINDOW_RESIZABLE
                                   | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!win) {
            continue;
        }
        gl = SDL_GL_CreateContext(win);
        if (gl) {
            *out_gl = gl;
            return win;
        }
        SDL_DestroyWindow(win);
    }
    return NULL;
}

int
main(int argc, char **argv)
{
    vapor_client        vc;
    vapor_app           app;
    SDL_Window         *win;
    SDL_GLContext       gl;
    struct nk_context  *ctx;
    struct nk_font_atlas *atlas;
    const char         *install_on_start = NULL;
    int                 rc = 0;
    int                 i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            install_on_start = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "vapor-gui: unexpected argument \"%s\"\n\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (vapor_client_open(&vc) != 0) {
        fprintf(stderr, "vapor-gui: %s\n", vc.err);
        return 1;
    }

    memset(&app, 0, sizeof(app));
    app.vc = &vc;
    seed_form(&app);

    /* SDL2main is deliberately not linked: this stays a plain console program
     * so libvapor's diagnostics remain visible while running the prototype. */
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "vapor-gui: SDL_Init: %s\n", SDL_GetError());
        vapor_client_close(&vc);
        return 1;
    }

    win = open_gl_window(&gl);
    if (!win || !gl) {
#if defined(VAPOR_TARGET_XP)
        /* This image is subsystem windows, so a failed start would otherwise
         * vanish. A console exists only long enough to show the error. */
        AllocConsole();
        freopen("CONOUT$", "w", stderr);
#endif
        fprintf(stderr, "vapor-gui: could not open an OpenGL window: %s\n",
                SDL_GetError());
#if defined(VAPOR_TARGET_XP)
        fprintf(stderr, "vapor-gui: this XP build needs a working OpenGL "
                        "driver; use vapor.exe if the window cannot start\n");
        fprintf(stderr, "press Enter to close\n");
        (void)getchar();
#endif
        if (win) {
            SDL_DestroyWindow(win);
        }
        SDL_Quit();
        vapor_client_close(&vc);
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    ctx = nk_sdl_init(win);
    nk_sdl_font_stash_begin(&atlas);
    /* The built-in font at 16px; nothing to ship alongside the binary. */
    {
        struct nk_font *font = nk_font_atlas_add_default(atlas, 16.0f, NULL);
        nk_sdl_font_stash_end();
        if (font) {
            nk_style_set_font(ctx, &font->handle);
        }
    }
    vapor_gui_style(ctx);

    vapor_gui_job_init(&app);

    /* A stored token is not a session until vapord confirms it. The library
     * screen is only shown after a live /me, so a dead server cannot skip the
     * login form. */
    if (vapor_client_has_token(&vc)
        && vapor_auth_whoami(&vc, &app.account) == 0) {
        app.screen = SCREEN_LIBRARY;
        vapor_gui_start_refresh(&app);
    } else {
        vapor_server_info info;

        app.screen = SCREEN_LOGIN;
        if (vapor_client_has_token(&vc)) {
            vapor_gui_notice(&app, 1, "could not restore the session: %s",
                             vc.err[0] ? vc.err : "server unreachable");
        }
        if (vapor_require_server(&vc, &info) == 0) {
            snprintf(app.server_info, sizeof(app.server_info),
                     "%s %s is reachable%s",
                     info.service[0] ? info.service : "server", info.version,
                     !info.registration_open ? "; registration is closed"
                     : info.has_users        ? ""
                                             : "; the first account becomes admin");
        } else {
            snprintf(app.server_info, sizeof(app.server_info),
                     "cannot reach the server; sign-in and account creation "
                     "need a live connection");
        }
    }

    while (!app.quit) {
        SDL_Event evt;
        int       w, h;

        nk_input_begin(ctx);
        /* Wait for input rather than spinning, but wake up regularly while a
         * job runs so the progress bar keeps moving. */
        if (SDL_WaitEventTimeout(&evt, vapor_gui_job_busy(&app) ? FRAME_MS : 200)) {
            do {
                if (evt.type == SDL_QUIT) {
                    app.quit = 1;
                }
                nk_sdl_handle_event(&evt);
            } while (SDL_PollEvent(&evt));
        }
        nk_sdl_handle_grab();
        nk_input_end(ctx);

        vapor_gui_job_poll(&app);

        /* Deferred until the startup catalog refresh has finished, because only
         * one job may own the client at a time. */
        if (install_on_start && app.screen == SCREEN_LIBRARY
            && !vapor_gui_job_busy(&app)) {
            if (vapor_gui_start_install(&app, install_on_start, "latest") == 0) {
                install_on_start = NULL;
            }
        }

        SDL_GetWindowSize(win, &w, &h);
        if (app.screen == SCREEN_LOGIN) {
            vapor_gui_login_screen(ctx, &app, w, h);
        } else {
            vapor_gui_library_screen(ctx, &app, w, h);
        }

        glViewport(0, 0, w, h);
        glClearColor(22 / 255.0f, 26 / 255.0f, 32 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_GL_SwapWindow(win);
    }

    /* Order matters: the worker has to be gone before the client it uses is
     * closed. */
    vapor_gui_job_shutdown(&app);
    vapor_gui_covers_release(&app);
    free(app.games);
    free(app.pending_games);
    free(app.pending_covers);
    vapor_secure_zero(app.password, sizeof(app.password));

    nk_sdl_shutdown();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    vapor_client_close(&vc);
    return rc;
}
