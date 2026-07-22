/* SPDX-License-Identifier: MIT */

#include "framebuffer.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------- synthetic tiles - */

void tess_fb_tiles_init(tess_fb_tiles *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

void tess_fb_tiles_add_missing(tess_fb_tiles *state, tess_tile tile)
{
    if (state == NULL || state->missing_count >= (uint8_t)(sizeof(state->missing) / sizeof(state->missing[0])))
    {
        return;
    }
    state->missing[state->missing_count++] = tile;
}

/* A colour that is a pure function of the tile address, so a rendered frame can
 * be checked against the tile that should be at a given place. Multipliers are
 * odd and mutually prime so neighbouring tiles differ in every channel. */
static void tile_colour(tess_tile tile, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint32_t h = (uint32_t) tile.x * 2654435761u
                     ^ (uint32_t) tile.y * 2246822519u
                     ^ (uint32_t) tile.zoom * 3266489917u;

    /* Kept in the upper half of the range so the darker checker square stays
     * distinguishable from the placeholder fill. */
    *r = (uint8_t)(128u + ((h >> 0) & 0x7Fu));
    *g = (uint8_t)(128u + ((h >> 8) & 0x7Fu));
    *b = (uint8_t)(128u + ((h >> 16) & 0x7Fu));
}

/* Pack 8-bit RGB into RGB565, the 16-bit layout most small colour panels
 * take directly. */
static uint16_t pack_a565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((((uint16_t) r >> 3) << 11)
                    | (((uint16_t) g >> 2) << 5)
                    | ((uint16_t) b >> 3));
}

static void unpack_a565(uint16_t value, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Replicate the high bits into the low ones so that full-scale in 5 or 6
     * bits maps to 255 rather than 248; a plain shift makes every rendered
     * white slightly grey and would make an exact pixel comparison in the
     * tests depend on which direction the error went. */
    const uint8_t r5 = (uint8_t)((value >> 11) & 0x1Fu);
    const uint8_t g6 = (uint8_t)((value >> 5) & 0x3Fu);
    const uint8_t b5 = (uint8_t)(value & 0x1Fu);

    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static tess_status host_load(void *ctx, tess_tile tile, void *image)
{
    tess_fb_tiles *state = (tess_fb_tiles *) ctx;
    if (state == NULL || image == NULL)
    {
        return TESS_ERR_ARG;
    }

    state->load_calls++;

    for (uint8_t i = 0; i < state->missing_count; i++)
    {
        if (tess_tile_equal(state->missing[i], tile))
        {
            return TESS_ERR_NOT_FOUND;
        }
    }
    if (state->io_error_after != 0u && state->load_calls > state->io_error_after)
    {
        return TESS_ERR_IO;
    }

    uint8_t r = 0, g = 0, b = 0;
    tile_colour(tile, &r, &g, &b);

    const uint16_t bright = pack_a565(r, g, b);
    const uint16_t dark = pack_a565((uint8_t)(r / 2u), (uint8_t)(g / 2u), (uint8_t)(b / 2u));

    uint16_t *pixels = (uint16_t *) image;

    for (int32_t y = 0; y < TESSERA_TILE_SIZE; y++)
    {
        for (int32_t x = 0; x < TESSERA_TILE_SIZE; x++)
        {
            /* 32-pixel checks, plus a one-pixel border so tile seams are
             * visible in a rendered frame and a misplacement of a single tile
             * shows up by eye. */
            const bool border = (x == 0) || (y == 0)
                             || (x == TESSERA_TILE_SIZE - 1) || (y == TESSERA_TILE_SIZE - 1);
            const bool check = (((x / 32) + (y / 32)) % 2) == 0;

            pixels[y * TESSERA_TILE_SIZE + x] = border ? pack_a565(0, 0, 0)
                                                       : (check ? bright : dark);
        }
    }
    return TESS_OK;
}

tess_tile_source tess_fb_tile_source(tess_fb_tiles *state)
{
    tess_tile_source source;
    source.ctx = state;
    source.load = host_load;
    return source;
}

/* ------------------------------------------------------------- framebuffer - */

tess_status tess_canvas_init(tess_canvas *canvas, uint8_t *rgb, int32_t width, int32_t height)
{
    if (canvas == NULL || rgb == NULL || width <= 0 || height <= 0)
    {
        return TESS_ERR_ARG;
    }

    memset(canvas, 0, sizeof(*canvas));
    canvas->rgb = rgb;
    canvas->width = width;
    canvas->height = height;
    memset(rgb, 0, (size_t)(width * height * 3));
    return TESS_OK;
}

static void put_pixel(tess_canvas *canvas, int32_t x, int32_t y,
                      uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
    {
        return;  /* clipping lives here, as it would in any real painter */
    }

    uint8_t *p = &canvas->rgb[(y * canvas->width + x) * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

void tess_canvas_get(const tess_canvas *canvas, int32_t x, int32_t y,
                        uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t out[3] = {0, 0, 0};

    if (canvas != NULL && x >= 0 && y >= 0 && x < canvas->width && y < canvas->height)
    {
        memcpy(out, &canvas->rgb[(y * canvas->width + x) * 3], 3);
    }

    if (r != NULL) { *r = out[0]; }
    if (g != NULL) { *g = out[1]; }
    if (b != NULL) { *b = out[2]; }
}

static void host_begin(void *ctx)
{
    tess_canvas *canvas = (tess_canvas *) ctx;

    canvas->tiles_drawn = 0;
    canvas->placeholders_drawn = 0;
    canvas->markers_drawn = 0;
    canvas->arrows_drawn = 0;
    memset(canvas->rgb, 0, (size_t)(canvas->width * canvas->height * 3));
}

static void host_draw_tile(void *ctx, const void *image, tess_tile tile, tess_point origin)
{
    tess_canvas *canvas = (tess_canvas *) ctx;
    const uint16_t *pixels = (const uint16_t *) image;

    (void) tile;
    canvas->tiles_drawn++;

    for (int32_t y = 0; y < TESSERA_TILE_SIZE; y++)
    {
        const int32_t sy = origin.y + y;
        if (sy < 0 || sy >= canvas->height)
        {
            continue;  /* whole row is off-screen: skip 256 bounds checks */
        }

        for (int32_t x = 0; x < TESSERA_TILE_SIZE; x++)
        {
            uint8_t r = 0, g = 0, b = 0;
            unpack_a565(pixels[y * TESSERA_TILE_SIZE + x], &r, &g, &b);
            put_pixel(canvas, origin.x + x, sy, r, g, b);
        }
    }
}

static void host_draw_placeholder(void *ctx, tess_tile tile, tess_point origin)
{
    tess_canvas *canvas = (tess_canvas *) ctx;

    (void) tile;
    canvas->placeholders_drawn++;

    /* A flat dark grey with a lighter outline -- visibly not a tile, and
     * exactly tile-sized, so a missing tile is obvious in a rendered frame. */
    for (int32_t y = 0; y < TESSERA_TILE_SIZE; y++)
    {
        const int32_t sy = origin.y + y;
        if (sy < 0 || sy >= canvas->height)
        {
            continue;
        }

        for (int32_t x = 0; x < TESSERA_TILE_SIZE; x++)
        {
            const bool edge = (x < 2) || (y < 2)
                           || (x >= TESSERA_TILE_SIZE - 2) || (y >= TESSERA_TILE_SIZE - 2);
            const uint8_t v = edge ? 72u : 40u;
            put_pixel(canvas, origin.x + x, sy, v, v, v);
        }
    }
}

/* A filled disc, used for both markers and edge arrows so that the painter has
 * no bitmap assets to carry. Real ports blit a rotated glyph here. */
static void draw_disc(tess_canvas *canvas, tess_point centre, int32_t radius,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int32_t dy = -radius; dy <= radius; dy++)
    {
        for (int32_t dx = -radius; dx <= radius; dx++)
        {
            if (dx * dx + dy * dy <= radius * radius)
            {
                put_pixel(canvas, centre.x + dx, centre.y + dy, r, g, b);
            }
        }
    }
}

static void host_draw_marker(void *ctx, const tess_marker *marker, tess_marker_placement placement,
                             uint8_t index)
{
    tess_canvas *canvas = (tess_canvas *) ctx;

    (void) marker;

    if (placement.on_screen)
    {
        canvas->markers_drawn++;
        /* The focus marker red, everything else yellow -- enough to tell them
         * apart in
         * a rendered frame. */
        if (index == TESS_MARKER_FOCUS)
        {
            draw_disc(canvas, placement.point, 6, 255, 32, 32);
        }
        else
        {
            draw_disc(canvas, placement.point, 5, 255, 216, 0);
        }
    }
    else
    {
        canvas->arrows_drawn++;
        draw_disc(canvas, placement.point, 4, 0, 200, 255);
    }
}

tess_painter tess_canvas_painter(tess_canvas *canvas)
{
    tess_painter painter;
    painter.ctx = canvas;
    painter.begin = host_begin;
    painter.draw_tile = host_draw_tile;
    painter.draw_placeholder = host_draw_placeholder;
    painter.draw_marker = host_draw_marker;
    painter.end = NULL;
    return painter;
}

tess_status tess_canvas_write_ppm(const tess_canvas *canvas, const char *path)
{
    if (canvas == NULL || path == NULL)
    {
        return TESS_ERR_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        return TESS_ERR_IO;
    }

    fprintf(f, "P6\n%ld %ld\n255\n", (long) canvas->width, (long) canvas->height);

    const size_t bytes = (size_t)(canvas->width * canvas->height * 3);
    const size_t written = fwrite(canvas->rgb, 1, bytes, f);

    /* fclose can fail on a flush, so a file that reported success can still be
     * truncated unless the close is checked too. */
    const bool ok = (written == bytes) && (fclose(f) == 0);
    return ok ? TESS_OK : TESS_ERR_IO;
}
