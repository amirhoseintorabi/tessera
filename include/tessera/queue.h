/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_QUEUE_H
#define TESSERA_QUEUE_H

/*
 * The tile request queue: what the view wants loaded, in the order it wants it,
 * drained by whatever is doing the reading.
 *
 * A bounded ring of tile addresses with no locking and no I/O in it, so it can
 * be tested exhaustively on a workstation. Serialising access belongs to the
 * caller; tess_map does it with one mutex around every touch.
 *
 * Two properties worth stating, because both are easy to leave out and neither
 * announces itself when missing:
 *
 *   - push() deduplicates. A viewport update recomputes the whole wanted set,
 *     so without this a slow medium means the reader spends its time fetching
 *     tiles it already has.
 *   - Nothing here reports success without writing its output parameter. A
 *     search that returns true on a miss hands the caller an uninitialised
 *     index, and the caller will use it as one.
 */

#include "tessera/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Capacity. The queue only ever holds the tiles for one viewport, so it needs
 * to be at least the grid size -- 35 for the largest grid this library allows.
 * 48 leaves headroom without being worth tuning, and costs 576 bytes.
 */
#ifndef TESS_QUEUE_CAPACITY
#define TESS_QUEUE_CAPACITY 48
#endif

typedef struct
{
    tess_tile items[TESS_QUEUE_CAPACITY];
    uint16_t head;  /* index of the next tile to pop */
    uint16_t count;
} tess_queue;

void tess_queue_init(tess_queue *q);
void tess_queue_clear(tess_queue *q);

uint16_t tess_queue_count(const tess_queue *q);
bool tess_queue_is_empty(const tess_queue *q);
bool tess_queue_is_full(const tess_queue *q);

/*
 * True if `tile` is queued. When it is, and `out_position` is non-NULL, the
 * position from the head is written there -- 0 meaning "next to be popped".
 *
 * `out_position` is written if and only if the function returns true.
 */
bool tess_queue_contains(const tess_queue *q, tess_tile tile, uint16_t *out_position);

/*
 * Append `tile`.
 *
 * TESS_OK if it was appended, TESS_OK also if it was already queued (the request
 * is satisfied either way, and callers should not have to distinguish),
 * TESS_ERR_FULL if there is no room, TESS_ERR_ARG for a tile address that cannot
 * exist.
 */
tess_status tess_queue_push(tess_queue *q, tess_tile tile);

/* Take the oldest tile. TESS_ERR_EMPTY leaves *out_tile untouched. */
tess_status tess_queue_pop(tess_queue *q, tess_tile *out_tile);

/* Look at the oldest tile without removing it. */
tess_status tess_queue_peek(const tess_queue *q, tess_tile *out_tile);

/* Drop `tile` wherever it sits, preserving the order of the rest. */
tess_status tess_queue_remove(tess_queue *q, tess_tile tile);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_QUEUE_H */
