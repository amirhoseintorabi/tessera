/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PORT_FRAMEBUFFER_H
#define TESSERA_PORT_FRAMEBUFFER_H

/*
 * A tile source and a painter that run on a workstation.
 *
 * These are not a mock in the "stand in for the real thing so the test passes"
 * sense. They are a second, independent port of the same two interfaces the
 * emWin and FatFs ports implement, which is what makes the engine's behaviour
 * observable: the painter renders to an RGB framebuffer that can be written
 * out as a PPM and looked at, and the source can be told to fail so that the
 * error paths are exercised rather than assumed.
 *
 * Tiles are RGB565: 16 bits per pixel, the layout most small colour panels
 * take directly, so what is drawn here goes through the same unpacking
 * arithmetic a real display port would.
 */

#include "tessera/map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- synthetic tiles -- */

typedef struct
{
    /* Tile addresses for which load() reports TESS_ERR_NOT_FOUND, so the
     * placeholder path can be driven deliberately. */
    tess_tile missing[8];
    uint8_t missing_count;

    /* When set, every load() after the first `io_error_after` calls returns
     * TESS_ERR_IO. 0 disables it. */
    uint32_t io_error_after;

    uint32_t load_calls;
} tess_fb_tiles;

void tess_fb_tiles_init(tess_fb_tiles *state);
void tess_fb_tiles_add_missing(tess_fb_tiles *state, tess_tile tile);

/* Bind a tile source to `state`. The source generates a checkerboard whose
 * colour is a hash of the tile address, so two different tiles are always
 * visibly different and the same tile is always the same colour. */
tess_tile_source tess_fb_tile_source(tess_fb_tiles *state);

/* ------------------------------------------------------------- framebuffer - */

typedef struct
{
    uint8_t *rgb;  /* width * height * 3, caller-owned */
    int32_t width;
    int32_t height;

    /* Counted so tests can assert on what was drawn without inspecting
     * pixels. */
    uint32_t tiles_drawn;
    uint32_t placeholders_drawn;
    uint32_t markers_drawn;
    uint32_t arrows_drawn;
} tess_canvas;

tess_status tess_canvas_init(tess_canvas *canvas, uint8_t *rgb, int32_t width, int32_t height);
tess_painter tess_canvas_painter(tess_canvas *canvas);

/* Write the canvas out as a binary PPM. Returns TESS_ERR_IO if the file cannot
 * be written. */
tess_status tess_canvas_write_ppm(const tess_canvas *canvas, const char *path);

/* Read one pixel, for tests that need to check a specific position. Out-of
 * range coordinates read as black. */
void tess_canvas_get(const tess_canvas *canvas, int32_t x, int32_t y,
                        uint8_t *r, uint8_t *g, uint8_t *b);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PORT_FRAMEBUFFER_H */
