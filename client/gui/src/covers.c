/* Cover-art textures for the library grid.
 *
 * Decoding and GL upload happen on the UI thread: the worker only writes a
 * cached file path. stb_image sniffs PNG/JPEG from the bytes, so the client
 * never has to trust the extension the publisher used. */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_HDR
#define STBI_NO_LINEAR

#include <stdlib.h>
#include <string.h>

#include "gui.h"

#include <SDL_opengl.h>

#include "stb_image.h"

static void
delete_texture(unsigned *tex)
{
    if (tex && *tex) {
        GLuint name = (GLuint)*tex;
        glDeleteTextures(1, &name);
        *tex = 0;
    }
}

void
vapor_gui_covers_release(vapor_app *app)
{
    size_t i;

    if (app->covers) {
        for (i = 0; i < app->ngames; i++) {
            delete_texture(&app->covers[i].tex);
        }
        free(app->covers);
        app->covers = NULL;
    }
}

void
vapor_gui_covers_adopt_pending(vapor_app *app)
{
    vapor_gui_covers_release(app);
    free(app->games);
    app->games = app->pending_games;
    app->covers = app->pending_covers;
    app->ngames = app->pending_ngames;
    app->pending_games = NULL;
    app->pending_covers = NULL;
    app->pending_ngames = 0;
    app->catalog_loaded = 1;
}

int
vapor_gui_cover_image(vapor_app *app, size_t index, struct nk_image *out)
{
    vapor_cover   *c;
    unsigned char *px;
    int            w = 0, h = 0, n = 0;
    GLuint         tex = 0;

    if (!out || !app->covers || index >= app->ngames) {
        return 0;
    }
    c = &app->covers[index];
    if (c->failed || !c->path[0]) {
        return 0;
    }
    if (c->tex) {
        *out = nk_image_id((int)c->tex);
        return 1;
    }

    px = stbi_load(c->path, &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        c->failed = 1;
        if (px) {
            stbi_image_free(px);
        }
        return 0;
    }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px);
    stbi_image_free(px);

    c->tex = (unsigned)tex;
    c->w = w;
    c->h = h;
    *out = nk_image_id((int)c->tex);
    return 1;
}
