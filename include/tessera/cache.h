/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_CACHE_H
#define TESSERA_CACHE_H

/*
 * The decoded-tile cache: a fixed set of slots, each holding one tile image.
 *
 * The cache does not know what an image is. Each slot carries a `void *image`
 * that the application points at its own buffer, so the pixel format, the size
 * and where the memory lives stay the application's decisions. On a small part
 * that matters more than it sounds: tile buffers are usually the largest single
 * allocation in the firmware, and often have to be placed in a particular
 * region by the linker script.
 *
 * A slot is in exactly one of three states:
 *
 *   FREE     nothing here, and nothing is coming
 *   LOADING  reserved by the loader, contents undefined, must not be drawn
 *   READY    holds the image for `tile`, safe to draw
 *
 * The transitions are the whole point of this module:
 *
 *   acquire()  FREE or evicted-READY -> LOADING   (before any I/O starts)
 *   commit()   LOADING -> READY                   (after the image is complete)
 *   release()  LOADING -> FREE                    (the load failed)
 *
 * Three things fall out of that separation, and each is a bug class that
 * cannot be written here:
 *
 *   - The reservation comes before any I/O, and cannot be skipped, because the
 *     reader has nowhere to put bytes until it succeeds. So the "no room"
 *     exit -- which a full cache makes routine -- is taken before anything has
 *     been opened, and cannot leak a handle.
 *   - A slot becomes visible to the drawing side at exactly one point,
 *     commit(), and only after its image is complete. There is no window in
 *     which a partly-decoded image is findable.
 *   - Nothing has to remember to return a slot to the pool. "Still wanted" is
 *     a timestamp the evictor compares, not a flag someone clears, so a slot
 *     that stops being wanted becomes reusable by doing nothing at all.
 */

#include "tessera/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TESS_SLOT_FREE = 0,
    TESS_SLOT_LOADING,
    TESS_SLOT_READY
} tess_slot_state;

typedef struct
{
    tess_slot_state state;
    tess_tile tile;      /* meaningful in LOADING and READY */
    uint32_t used_at;  /* frame number of the last touch; the LRU key */
    void *image;       /* application-owned; never dereferenced here */
} tess_slot;

typedef struct
{
    tess_slot *slots;
    uint16_t count;
    uint32_t frame;  /* incremented by tess_cache_begin_frame */
} tess_cache;

/*
 * Bind a cache to an array of `count` slots.
 *
 * `slots[i].image` must already point at a buffer; everything else is reset.
 * Returns TESS_ERR_ARG for a NULL array or a zero count, and TESS_ERR_ARG if any
 * slot has a NULL image -- a cache with nowhere to decode into is a
 * configuration mistake worth catching at start-up rather than at the first
 * pan.
 */
tess_status tess_cache_init(tess_cache *cache, tess_slot *slots, uint16_t count);

/* Start a new frame. The LRU key advances, so anything not touched during this
 * frame becomes a candidate for eviction ahead of anything that was. */
void tess_cache_begin_frame(tess_cache *cache);

/*
 * Index of the READY slot holding `tile`, or -1.
 *
 * Does not touch the slot: a lookup made while deciding what to draw should
 * not change eviction order on its own. Call tess_cache_touch when the tile is
 * actually wanted for this frame.
 */
int tess_cache_find(const tess_cache *cache, tess_tile tile);

/* As tess_cache_find, but also matches slots that are still LOADING. This is
 * what stops the same tile being queued twice while its read is in flight. */
int tess_cache_find_pending(const tess_cache *cache, tess_tile tile);

/* Mark a slot as wanted in the current frame. Out-of-range indices are
 * ignored, so the result of a failed find can be passed straight in. */
void tess_cache_touch(tess_cache *cache, int index);

/*
 * Reserve a slot for `tile` and put it in LOADING.
 *
 * Prefers a FREE slot; otherwise evicts the READY slot with the oldest
 * `used_at`. LOADING slots are never evicted -- something is writing into
 * them.
 *
 * TESS_ERR_FULL means every slot is in flight, which is a transient condition
 * the loader should retry rather than an error. If `tile` is already present,
 * READY or LOADING, its index is returned with status TESS_OK and no state is
 * changed.
 */
tess_status tess_cache_acquire(tess_cache *cache, tess_tile tile, int *out_index);

/* Publish a completed image: LOADING -> READY, touched as of this frame. */
tess_status tess_cache_commit(tess_cache *cache, int index);

/* Abandon a reservation after a failed load: LOADING -> FREE. */
tess_status tess_cache_release(tess_cache *cache, int index);

/* Drop everything, FREE and LOADING alike. Used on zoom changes only if the
 * caller wants the memory back immediately; ordinary operation relies on
 * eviction, which keeps the old zoom's tiles usable while the new ones load. */
void tess_cache_reset(tess_cache *cache);

/* How many slots are in each state. Any of the outputs may be NULL. */
void tess_cache_stats(const tess_cache *cache, uint16_t *out_free, uint16_t *out_loading,
                    uint16_t *out_ready);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_CACHE_H */
