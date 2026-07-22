/* SPDX-License-Identifier: MIT */

#include "tessera/map.h"

#include <string.h>

/* ------------------------------------------------------------------ locking */

static void lock_acquire(const tess_map *map)
{
    if (map->lock != NULL && map->lock->acquire != NULL)
    {
        map->lock->acquire(map->lock->ctx);
    }
}

static void lock_release(const tess_map *map)
{
    if (map->lock != NULL && map->lock->release != NULL)
    {
        map->lock->release(map->lock->ctx);
    }
}

/* --------------------------------------------------------------- lifecycle - */

tess_status tess_map_init(tess_map *map, const tess_map_config *config)
{
    if (map == NULL || config == NULL)
    {
        return TESS_ERR_ARG;
    }

    memset(map, 0, sizeof(*map));

    tess_status status = tess_view_init(&map->view, config->centre, config->zoom,
                                    config->width, config->height);
    if (status != TESS_OK)
    {
        return status;
    }

    if (config->grid_cols == 0u && config->grid_rows == 0u)
    {
        status = tess_grid_init_for_viewport(&map->grid, config->width, config->height);
    }
    else
    {
        status = tess_grid_init(&map->grid, config->grid_cols, config->grid_rows);
    }
    if (status != TESS_OK)
    {
        return status;
    }

    /* A grid larger than the cache would evict a tile it is about to ask for
     * again on the very next frame, and the map would thrash without ever
     * settling. Catching it here turns a baffling runtime symptom into a
     * start-up error with an obvious cause. */
    if (config->slot_count < tess_grid_count(&map->grid))
    {
        return TESS_ERR_ARG;
    }

    status = tess_cache_init(&map->cache, config->slots, config->slot_count);
    if (status != TESS_OK)
    {
        return status;
    }

    tess_queue_init(&map->queue);

    map->source = config->source;
    map->lock = config->lock;
    map->marker_edge_inset = config->marker_edge_inset;
    map->marker_count = 0;

    tess_map_refresh(map);
    return TESS_OK;
}

/* ----------------------------------------------------------------- viewport */

void tess_map_refresh(tess_map *map)
{
    if (map == NULL)
    {
        return;
    }

    const tess_tile centre = tess_view_centre_tile(&map->view);
    const uint8_t count = tess_grid_count(&map->grid);

    lock_acquire(map);

    /* A new frame for the LRU, and a queue emptied of the previous view's
     * wants. Anything still needed is re-queued below, in the new view's
     * priority order -- which is the point of clearing rather than filtering.
     * A tile already in flight is not in the queue (tess_map_service popped it),
     * so nothing in progress is lost. */
    tess_cache_begin_frame(&map->cache);
    tess_queue_clear(&map->queue);

    for (uint8_t n = 0; n < count; n++)
    {
        uint8_t col = 0, row = 0;
        if (tess_grid_order_at(&map->grid, n, &col, &row) != TESS_OK)
        {
            continue;
        }

        const tess_tile tile = tess_grid_tile_at(&map->grid, centre, col, row);
        if (!tess_tile_is_valid(tile))
        {
            continue;  /* off the top or bottom of the world: nothing to load */
        }

        const int slot = tess_cache_find(&map->cache, tile);
        if (slot >= 0)
        {
            tess_cache_touch(&map->cache, slot);
            continue;
        }

        /* Still loading: wanted, but asking again would just duplicate work. */
        if (tess_cache_find_pending(&map->cache, tile) >= 0)
        {
            continue;
        }

        if (tess_queue_push(&map->queue, tile) == TESS_ERR_FULL)
        {
            map->stats.queue_overflows++;
        }
        else
        {
            map->stats.tiles_requested++;
        }
    }

    lock_release(map);
}

void tess_map_set_centre(tess_map *map, tess_geo centre)
{
    if (map == NULL)
    {
        return;
    }
    tess_view_set_centre(&map->view, centre);
    tess_map_refresh(map);
}

void tess_map_pan(tess_map *map, int32_t dx, int32_t dy)
{
    if (map == NULL)
    {
        return;
    }
    tess_view_pan(&map->view, dx, dy);
    tess_map_refresh(map);
}

void tess_map_zoom_at(tess_map *map, int delta, tess_point anchor)
{
    if (map == NULL)
    {
        return;
    }
    tess_view_zoom_at(&map->view, delta, anchor);
    tess_map_refresh(map);
}

void tess_map_zoom(tess_map *map, int delta)
{
    if (map == NULL)
    {
        return;
    }

    const tess_point centre = {map->view.width / 2, map->view.height / 2};
    tess_map_zoom_at(map, delta, centre);
}

tess_status tess_map_set_size(tess_map *map, int32_t width, int32_t height)
{
    if (map == NULL)
    {
        return TESS_ERR_ARG;
    }

    tess_status status = tess_view_set_size(&map->view, width, height);
    if (status != TESS_OK)
    {
        return status;
    }

    /* A larger widget may need a larger grid. Keep the old one if the new one
     * would not fit the cache, rather than failing a resize the caller has
     * already committed to on the display side. */
    tess_grid resized;
    if (tess_grid_init_for_viewport(&resized, width, height) == TESS_OK
        && tess_grid_count(&resized) <= map->cache.count)
    {
        map->grid = resized;
    }

    tess_map_refresh(map);
    return TESS_OK;
}

tess_geo tess_map_screen_to_geo(const tess_map *map, tess_point point)
{
    tess_geo position = {0.0, 0.0};
    if (map == NULL)
    {
        return position;
    }
    return tess_view_screen_to_geo(&map->view, point);
}

/* ------------------------------------------------------------------ markers */

static bool marker_index_ok(const tess_map *map, uint8_t index)
{
    return map != NULL && index < TESS_MARKER_MAX;
}

/* Markers are a fixed array with a high-water mark, not a list.
 *
 * That keeps an index stable for the lifetime of the map: whoever owns marker 3
 * can keep updating marker 3 without caring what happened to markers 1 and 2.
 * A compacting list would renumber on every removal, and the caller would have
 * to track the renumbering to keep pointing at the right thing. */
static void note_marker_used(tess_map *map, uint8_t index)
{
    if (index >= map->marker_count)
    {
        map->marker_count = (uint8_t)(index + 1u);
    }
}

tess_status tess_map_marker_set(tess_map *map, uint8_t index, tess_geo position, const char *label)
{
    if (!marker_index_ok(map, index))
    {
        return TESS_ERR_ARG;
    }

    tess_marker_set(&map->markers[index], position, label);
    note_marker_used(map, index);
    return TESS_OK;
}

tess_status tess_map_marker_set_heading(tess_map *map, uint8_t index, int heading_deg)
{
    if (!marker_index_ok(map, index) || index >= map->marker_count)
    {
        return TESS_ERR_ARG;
    }

    tess_marker_set_heading(&map->markers[index], heading_deg);
    return TESS_OK;
}

tess_status tess_map_marker_set_visible(tess_map *map, uint8_t index, bool visible)
{
    if (!marker_index_ok(map, index) || index >= map->marker_count)
    {
        return TESS_ERR_ARG;
    }

    map->markers[index].visible = visible;
    return TESS_OK;
}

tess_status tess_map_marker_clear(tess_map *map, uint8_t index)
{
    if (!marker_index_ok(map, index))
    {
        return TESS_ERR_ARG;
    }

    memset(&map->markers[index], 0, sizeof(map->markers[index]));

    /* Pull the high-water mark back over any trailing cleared entries, so a
     * cleared marker stops being iterated rather than merely being invisible. */
    while (map->marker_count > 0u && !map->markers[map->marker_count - 1u].visible)
    {
        map->marker_count--;
    }
    return TESS_OK;
}

void tess_map_markers_clear(tess_map *map)
{
    if (map == NULL)
    {
        return;
    }
    memset(map->markers, 0, sizeof(map->markers));
    map->marker_count = 0;
}

uint8_t tess_map_marker_count(const tess_map *map)
{
    return (map != NULL) ? map->marker_count : 0u;
}

const tess_marker *tess_map_marker(const tess_map *map, uint8_t index)
{
    if (!marker_index_ok(map, index) || index >= map->marker_count)
    {
        return NULL;
    }
    return &map->markers[index];
}

tess_status tess_map_marker_placement(const tess_map *map, uint8_t index, tess_marker_placement *out)
{
    if (out == NULL)
    {
        return TESS_ERR_ARG;
    }

    const tess_marker *marker = tess_map_marker(map, index);
    if (marker == NULL)
    {
        return TESS_ERR_ARG;
    }

    *out = tess_marker_locate(&map->view, marker->position, map->marker_edge_inset);
    return TESS_OK;
}

tess_status tess_map_fit_markers(tess_map *map, int32_t margin)
{
    if (map == NULL)
    {
        return TESS_ERR_ARG;
    }

    tess_geo positions[TESS_MARKER_MAX];
    int count = 0;

    for (uint8_t i = 0; i < map->marker_count; i++)
    {
        if (map->markers[i].visible)
        {
            positions[count++] = map->markers[i].position;
        }
    }

    if (count == 0)
    {
        return TESS_ERR_ARG;
    }

    const tess_status status = tess_view_fit(&map->view, positions, count, margin);
    if (status == TESS_OK)
    {
        tess_map_refresh(map);
    }
    return status;
}

/* ----------------------------------------------------------------- drawing - */

tess_status tess_map_draw(tess_map *map, const tess_painter *painter)
{
    if (map == NULL || painter == NULL)
    {
        return TESS_ERR_ARG;
    }

    if (painter->begin != NULL)
    {
        painter->begin(painter->ctx);
    }

    const tess_tile centre = tess_view_centre_tile(&map->view);

    for (uint8_t row = 0; row < map->grid.rows; row++)
    {
        for (uint8_t col = 0; col < map->grid.cols; col++)
        {
            const tess_tile tile = tess_grid_tile_at(&map->grid, centre, col, row);
            const tess_point origin = tess_view_tile_origin(&map->view, tile);

            /*
             * The cache is read under the lock and the image pointer copied
             * out, but the blit happens outside it.
             *
             * That is safe because a READY slot only becomes something else
             * through tess_cache_acquire, which the loader calls before it
             * writes -- and the worst case is a frame that draws a tile being
             * replaced, which is a torn tile for one frame at 10 Hz. Holding
             * the lock across the blit instead would block the loader for the
             * whole of the drawing pass, every frame.
             */
            const void *image = NULL;

            if (tess_tile_is_valid(tile))
            {
                lock_acquire(map);
                const int slot = tess_cache_find(&map->cache, tile);
                if (slot >= 0)
                {
                    tess_cache_touch(&map->cache, slot);
                    image = map->cache.slots[slot].image;
                }
                lock_release(map);
            }

            if (image != NULL && painter->draw_tile != NULL)
            {
                painter->draw_tile(painter->ctx, image, tile, origin);
            }
            else if (painter->draw_placeholder != NULL)
            {
                painter->draw_placeholder(painter->ctx, tile, origin);
            }
        }
    }

    if (painter->draw_marker != NULL)
    {
        for (uint8_t i = 0; i < map->marker_count; i++)
        {
            if (!map->markers[i].visible)
            {
                continue;
            }

            const tess_marker_placement placement =
                tess_marker_locate(&map->view, map->markers[i].position, map->marker_edge_inset);

            painter->draw_marker(painter->ctx, &map->markers[i], placement, i);
        }
    }

    if (painter->end != NULL)
    {
        painter->end(painter->ctx);
    }
    return TESS_OK;
}

/* ------------------------------------------------------------------ loader - */

tess_status tess_map_service(tess_map *map)
{
    if (map == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (map->source == NULL || map->source->load == NULL)
    {
        return TESS_ERR_EMPTY;
    }

    tess_tile tile;
    int slot = -1;

    /*
     * Phase 1, under the lock: take a request and reserve somewhere to put it.
     *
     * Reserving first is what makes the load path leak-proof. A full cache is
     * an ordinary condition, not an error, so the "nowhere to put this" exit
     * gets taken routinely -- and if the medium had already been opened by
     * then, that exit is a handle leak on a resource that is fixed in number.
     * Here there is nothing open yet to leak.
     */
    lock_acquire(map);

    /* Peek rather than pop: the request is only consumed once there is
     * somewhere to put the result. Popping first and pushing back on failure
     * would send the tile to the *tail* of the queue -- demoting the tile
     * nearest the view centre every time the cache happened to be busy. */
    tess_status status = tess_queue_peek(&map->queue, &tile);
    if (status == TESS_OK)
    {
        /*
         * Present already, or already in flight on another loader?
         *
         * This has to be asked before acquiring, not after: tess_cache_acquire
         * returns the existing slot in both cases, and from its return value
         * alone a caller cannot tell "you reserved this" from "someone else
         * did". Two loaders that both went on to read would write into the
         * same buffer at the same time.
         */
        const int existing = tess_cache_find_pending(&map->cache, tile);
        if (existing >= 0)
        {
            tess_cache_touch(&map->cache, existing);
            (void) tess_queue_pop(&map->queue, &tile);
        }
        else
        {
            status = tess_cache_acquire(&map->cache, tile, &slot);
            if (status != TESS_OK)
            {
                map->stats.cache_full++;  /* leave it queued; the caller retries */
                slot = -1;
            }
            else
            {
                (void) tess_queue_pop(&map->queue, &tile);
            }
        }
    }

    void *image = (slot >= 0) ? map->cache.slots[slot].image : NULL;

    lock_release(map);

    if (status != TESS_OK || slot < 0)
    {
        return status;
    }

    /*
     * Phase 2, with the lock released: the slow part.
     *
     * The slot is in LOADING, so nothing else will evict it or draw it while
     * this runs, and the GUI thread is free to keep painting the tiles it
     * already has.
     */
    const tess_status load_status = map->source->load(map->source->ctx, tile, image);

    /* Phase 3, under the lock again: publish, or give the slot back.
     *
     * This is the only point at which a slot becomes drawable, and it is
     * reached only after the image is complete. Publishing earlier -- setting a
     * "loaded" flag and then filling the buffer -- lets the drawing side find a
     * half-decoded image, and the window is exactly as wide as the decode. */
    lock_acquire(map);

    if (load_status == TESS_OK)
    {
        (void) tess_cache_commit(&map->cache, slot);
        map->stats.tiles_loaded++;
    }
    else
    {
        (void) tess_cache_release(&map->cache, slot);
        if (load_status == TESS_ERR_NOT_FOUND)
        {
            map->stats.tiles_missing++;
        }
        else
        {
            map->stats.tiles_failed++;
        }
    }

    lock_release(map);
    return load_status;
}

uint16_t tess_map_pending(const tess_map *map)
{
    if (map == NULL)
    {
        return 0;
    }

    uint16_t loading = 0;
    lock_acquire(map);
    const uint16_t queued = tess_queue_count(&map->queue);
    tess_cache_stats(&map->cache, NULL, &loading, NULL);
    lock_release(map);

    return (uint16_t)(queued + loading);
}

tess_map_stats tess_map_get_stats(const tess_map *map)
{
    tess_map_stats stats;
    memset(&stats, 0, sizeof(stats));

    if (map != NULL)
    {
        stats = map->stats;
    }
    return stats;
}
