/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_MAP_H
#define TESSERA_MAP_H

/*
 * The map engine: a viewport, a grid of tiles around it, a cache, a request
 * queue and a set of markers, with no dependency on any GUI toolkit, any
 * filesystem or any RTOS.
 *
 * There are two callers, and they are on different threads:
 *
 *   GUI side     tess_map_set_centre / pan / zoom / fit / marker_*  then
 *                tess_map_draw once per frame.
 *   Loader side  tess_map_service in a loop, blocking on whatever the port's
 *                idle hook does between calls.
 *
 * Everything either of them touches is behind the lock in tess_map_config. The
 * one thing deliberately outside it is the tile source's `load` -- the SD card
 * read -- because it is the slow part and holding a mutex across it would
 * stall drawing for the whole of it.
 *
 * Typical wiring:
 *
 *     static uint8_t buffers[15][TESS_TILE_BYTES_16BPP];
 *     static tess_slot slots[15];
 *     for (int i = 0; i < 15; i++) slots[i].image = buffers[i];
 *
 *     tess_map_config cfg = {0};
 *     cfg.centre = home;  cfg.zoom = 16;
 *     cfg.width  = 480;   cfg.height = 272;
 *     cfg.slots  = slots; cfg.slot_count = 15;
 *     cfg.source = &fatfs_source;
 *     cfg.lock   = &rtos_lock;
 *     tess_map_init(&map, &cfg);
 */

#include "tessera/cache.h"
#include "tessera/grid.h"
#include "tessera/marker.h"
#include "tessera/port.h"
#include "tessera/queue.h"
#include "tessera/types.h"
#include "tessera/view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Markers the map can hold. Ten covers a focus marker plus a route's worth of
 * waypoints; raise it at build time if you need more. */
#ifndef TESS_MARKER_MAX
#define TESS_MARKER_MAX 10
#endif

/*
 * Marker 0 is the focus marker by convention -- the thing the map is following,
 * usually drawn rotated to its heading and kept on top.
 *
 * Nothing in the engine enforces that. The name exists so that application code
 * and painters can agree on which index means "this one is special" without
 * either of them writing a bare 0.
 */
#define TESS_MARKER_FOCUS 0

/*
 * Bytes in one decoded tile, for the two pixel depths embedded displays
 * usually use. Buffers are sized by the application, so these are a
 * convenience rather than a constraint -- but sizing them from the wrong depth
 * is a large and silent waste: across 25 slots the difference is 3.1 MB.
 */
#define TESS_TILE_BYTES_16BPP (TESSERA_TILE_SIZE * TESSERA_TILE_SIZE * 2)
#define TESS_TILE_BYTES_32BPP (TESSERA_TILE_SIZE * TESSERA_TILE_SIZE * 4)

typedef struct
{
    tess_geo centre;
    int zoom;
    int32_t width;
    int32_t height;

    /*
     * Cache storage, owned by the caller. Each slot's `image` must already
     * point at a buffer big enough for one decoded tile.
     *
     * There must be at least as many slots as there are tiles in the grid --
     * fewer and the cache would evict a tile it is about to ask for again on
     * the next frame, and the map would thrash without ever settling.
     *
     * Twice the grid is the size worth paying for when the memory is there:
     * the tiles for a new zoom level land in the spare half, so zooming
     * straight back costs nothing instead of a full round of reads. At 16 bits
     * per pixel that is 128 KB per slot, so a 5x5 grid is 3.1 MB for one level
     * and 6.3 MB for two -- external memory either way on a small part.
     */
    tess_slot *slots;
    uint16_t slot_count;

    /* Grid size. Leave both at 0 to have it derived from the viewport, which
     * is what almost every caller wants. */
    uint8_t grid_cols;
    uint8_t grid_rows;

    /* How far in from the viewport edge off-screen marker arrows sit. 0 puts
     * them exactly on the edge, where half the glyph would be clipped. */
    int32_t marker_edge_inset;

    const tess_tile_source *source;
    const tess_lock *lock;  /* NULL for single-threaded use */
} tess_map_config;

typedef struct
{
    uint32_t tiles_requested;  /* pushed onto the queue                       */
    uint32_t tiles_loaded;     /* read and committed                          */
    uint32_t tiles_missing;    /* the medium had no such tile                 */
    uint32_t tiles_failed;     /* the medium errored                          */
    uint32_t queue_overflows;  /* a request dropped because the queue was full */
    uint32_t cache_full;       /* every slot in flight when one was needed    */
} tess_map_stats;

typedef struct
{
    tess_view view;
    tess_grid grid;
    tess_cache cache;
    tess_queue queue;

    tess_marker markers[TESS_MARKER_MAX];
    uint8_t marker_count;

    int32_t marker_edge_inset;

    const tess_tile_source *source;
    const tess_lock *lock;

    tess_map_stats stats;
} tess_map;

/* --------------------------------------------------------------- lifecycle - */

/*
 * Set up a map. Fails with TESS_ERR_ARG rather than half-initialising: a NULL
 * config, no slots, a slot with no buffer, a non-positive viewport, or a grid
 * size that is not odd.
 *
 * A tile source is *not* required. Without one the map draws placeholders,
 * which is a useful state during bring-up and is what the tests for the
 * geometry use.
 */
tess_status tess_map_init(tess_map *map, const tess_map_config *config);

/* -------------------------------------------------------------- GUI thread - */

void tess_map_set_centre(tess_map *map, tess_geo centre);
void tess_map_pan(tess_map *map, int32_t dx, int32_t dy);
void tess_map_zoom(tess_map *map, int delta);
void tess_map_zoom_at(tess_map *map, int delta, tess_point anchor);
tess_status tess_map_set_size(tess_map *map, int32_t width, int32_t height);

/* Frame every visible marker, with `margin` pixels kept clear at the edges. */
tess_status tess_map_fit_markers(tess_map *map, int32_t margin);

/* The position under a screen pixel -- for tap-to-place and drag-to-pan. */
tess_geo tess_map_screen_to_geo(const tess_map *map, tess_point point);

/*
 * Recompute which tiles the current view needs and queue the missing ones.
 *
 * Called automatically by every function above that moves the view, so an
 * application only calls it directly after changing the tile source or to
 * force a retry of tiles that failed.
 */
void tess_map_refresh(tess_map *map);

/* ------------------------------------------------------------------ markers */

tess_status tess_map_marker_set(tess_map *map, uint8_t index, tess_geo position, const char *label);
tess_status tess_map_marker_set_heading(tess_map *map, uint8_t index, int heading_deg);
tess_status tess_map_marker_set_visible(tess_map *map, uint8_t index, bool visible);
tess_status tess_map_marker_clear(tess_map *map, uint8_t index);
void tess_map_markers_clear(tess_map *map);
uint8_t tess_map_marker_count(const tess_map *map);
const tess_marker *tess_map_marker(const tess_map *map, uint8_t index);

/* Where a marker would be drawn, without drawing it. */
tess_status tess_map_marker_placement(const tess_map *map, uint8_t index, tess_marker_placement *out);

/* ----------------------------------------------------------------- drawing - */

/*
 * Draw one frame: every grid position, then every visible marker.
 *
 * Tiles are drawn in grid order -- top-left to bottom-right, not fetch order --
 * so overlapping edges compose the same way every frame. Markers are drawn
 * last, in index order, so marker 0 ends up beneath the others; a painter that
 * wants the focus marker on top can reorder, but the engine does not
 * presume.
 */
tess_status tess_map_draw(tess_map *map, const tess_painter *painter);

/* --------------------------------------------------------------- loader -- */

/*
 * Load at most one queued tile.
 *
 * Returns TESS_OK if a tile was loaded, TESS_ERR_EMPTY if the queue was empty,
 * TESS_ERR_FULL if every cache slot is in flight, and the source's own error
 * otherwise. All four are ordinary: the intended loop is
 *
 *     for (;;) {
 *         if (tess_map_service(&map) == TESS_ERR_EMPTY) osDelay(50);
 *     }
 *
 * with no other sleep in it. Sleeping after every tile rather than only when
 * the queue is empty puts a hard ceiling on the fill rate -- at 100 ms that is
 * ten tiles a second however fast the medium is, so a screenful takes two and
 * a half seconds of mostly waiting.
 */
tess_status tess_map_service(tess_map *map);

/* How many tiles the current view is still waiting for. */
uint16_t tess_map_pending(const tess_map *map);

tess_map_stats tess_map_get_stats(const tess_map *map);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_MAP_H */
