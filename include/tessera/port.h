/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PORT_H
#define TESSERA_PORT_H

/*
 * The three things this library needs from its host, as vtables: somewhere to
 * get tile images from, something to draw with, and a mutex.
 *
 * Everything platform-specific enters here and nowhere else. That is what lets
 * the engine run on a workstation against a synthetic tile source and a painter
 * that writes a PPM file, while the same engine drives a real display through a
 * binding that does nothing but translate one call into another.
 *
 * It is also the seam that decides how much of a map widget can be tested at
 * all. Call the graphics library, the filesystem and the scheduler directly
 * from the logic and the answer is none of it: checking whether the right tile
 * is at the right pixel then requires the display, the storage and the RTOS.
 */

#include "tessera/marker.h"
#include "tessera/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- tile source */

typedef struct
{
    void *ctx;

    /*
     * Fill `image` with the decoded tile at `tile`.
     *
     * `image` is the buffer the application put in the cache slot. Called from
     * the loader with the map's lock *not* held, because this is where the
     * tens of milliseconds of SD card latency are and holding a mutex across
     * it would stall the drawing side for exactly as long.
     *
     * Returns TESS_OK on success, TESS_ERR_NOT_FOUND when the medium has no such
     * tile (a normal condition near the edge of the mapped area, not an
     * error), or TESS_ERR_IO for anything else. The engine treats a non-TESS_OK
     * result by releasing the slot, so a failed read can never be drawn.
     */
    tess_status (*load)(void *ctx, tess_tile tile, void *image);
} tess_tile_source;

/* ------------------------------------------------------------------- painter */

typedef struct
{
    void *ctx;

    /* Optional. Called once before any tile is drawn. */
    void (*begin)(void *ctx);

    /* Blit a loaded tile so that its north-west corner lands on `origin`.
     * `origin` may be negative or beyond the viewport; clipping is the
     * painter's business, since every graphics library already has it. */
    void (*draw_tile)(void *ctx, const void *image, tess_tile tile, tess_point origin);

    /* Optional. Draw whatever stands in for a tile that has not arrived --
     * a flat fill, a grid, a label. Called with the same origin draw_tile
     * would have received, so a placeholder can be exactly tile-sized. */
    void (*draw_placeholder)(void *ctx, tess_tile tile, tess_point origin);

    /* Draw a marker. `placement.on_screen` distinguishes the marker itself
     * from the edge arrow that stands in for it; `placement.point` is where
     * the marker's anchor goes, not its top-left corner, so the painter
     * subtracts its own bitmap's half-size. */
    void (*draw_marker)(void *ctx, const tess_marker *marker, tess_marker_placement placement,
                        uint8_t index);

    /* Optional. Called once after everything else. */
    void (*end)(void *ctx);
} tess_painter;

/* ---------------------------------------------------------------------- lock */

/*
 * Mutual exclusion between the GUI side and the loader.
 *
 * Both may be NULL, meaning single-threaded, which is how the tests run. When
 * they are set, the engine takes the lock around every touch of the queue and
 * the cache and never holds it across a call into the tile source.
 *
 * Use the RTOS's own mutex. The hand-rolled alternative --
 *
 *     while (in_use) { sleep(10); }
 *     in_use = true;
 *
 * -- is not a lock. The test and the set are separate statements with a
 * scheduling point between them, so two threads can both observe false and
 * both proceed; and unless the flag is atomic the compiler is entitled to
 * hoist the load out of the wait loop and spin on a stale value forever.
 * `volatile` does not fix the second problem and does nothing about the first:
 * it constrains the compiler's caching of a value and says nothing about
 * ordering between threads.
 */
typedef struct
{
    void *ctx;
    void (*acquire)(void *ctx);
    void (*release)(void *ctx);
} tess_lock;

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PORT_H */
