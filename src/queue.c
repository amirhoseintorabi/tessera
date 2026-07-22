/* SPDX-License-Identifier: MIT */

#include "tessera/queue.h"

#include <string.h>

/* Ring arithmetic in one place. TESS_QUEUE_CAPACITY is not required to be a
 * power of two, so this is a conditional subtract rather than a mask -- which
 * is also cheaper than a division on a Cortex-M4 without a divider in the
 * relevant path. */
static uint16_t wrap(uint16_t index)
{
    return (index >= TESS_QUEUE_CAPACITY) ? (uint16_t)(index - TESS_QUEUE_CAPACITY) : index;
}

static uint16_t slot_of(const tess_queue *q, uint16_t offset)
{
    return wrap((uint16_t)(q->head + offset));
}

void tess_queue_init(tess_queue *q)
{
    if (q == NULL)
    {
        return;
    }
    memset(q, 0, sizeof(*q));
}

void tess_queue_clear(tess_queue *q)
{
    if (q == NULL)
    {
        return;
    }
    q->head = 0;
    q->count = 0;
    /* The item array is deliberately not cleared: nothing reads past `count`,
     * and clearing 576 bytes on every viewport change is real work on a 204 MHz
     * part. The tests check that stale entries are unreachable rather than
     * absent. */
}

uint16_t tess_queue_count(const tess_queue *q)
{
    return (q != NULL) ? q->count : 0u;
}

bool tess_queue_is_empty(const tess_queue *q)
{
    return tess_queue_count(q) == 0u;
}

bool tess_queue_is_full(const tess_queue *q)
{
    return tess_queue_count(q) >= TESS_QUEUE_CAPACITY;
}

bool tess_queue_contains(const tess_queue *q, tess_tile tile, uint16_t *out_position)
{
    if (q == NULL)
    {
        return false;
    }

    for (uint16_t i = 0; i < q->count; i++)
    {
        if (tess_tile_equal(q->items[slot_of(q, i)], tile))
        {
            if (out_position != NULL)
            {
                *out_position = i;
            }
            return true;
        }
    }
    return false;
}

tess_status tess_queue_push(tess_queue *q, tess_tile tile)
{
    if (q == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (!tess_tile_is_valid(tile))
    {
        return TESS_ERR_ARG;
    }
    /* Already wanted. Re-appending would push it behind tiles queued later,
     * which is exactly backwards: it was requested first because it is nearer
     * the centre of the view. */
    if (tess_queue_contains(q, tile, NULL))
    {
        return TESS_OK;
    }
    if (tess_queue_is_full(q))
    {
        return TESS_ERR_FULL;
    }

    q->items[slot_of(q, q->count)] = tile;
    q->count++;
    return TESS_OK;
}

tess_status tess_queue_pop(tess_queue *q, tess_tile *out_tile)
{
    if (q == NULL || out_tile == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (q->count == 0u)
    {
        return TESS_ERR_EMPTY;
    }

    *out_tile = q->items[q->head];
    q->head = wrap((uint16_t)(q->head + 1u));
    q->count--;
    return TESS_OK;
}

tess_status tess_queue_peek(const tess_queue *q, tess_tile *out_tile)
{
    if (q == NULL || out_tile == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (q->count == 0u)
    {
        return TESS_ERR_EMPTY;
    }

    *out_tile = q->items[q->head];
    return TESS_OK;
}

tess_status tess_queue_remove(tess_queue *q, tess_tile tile)
{
    if (q == NULL)
    {
        return TESS_ERR_ARG;
    }

    uint16_t position = 0;
    if (!tess_queue_contains(q, tile, &position))
    {
        return TESS_ERR_NOT_FOUND;
    }

    /* Shift the tail down by one. Element-by-element rather than memmove
     * because the region can be split by the ring's wrap point, and because
     * `count` is at most 48. */
    for (uint16_t i = position; i + 1u < q->count; i++)
    {
        q->items[slot_of(q, i)] = q->items[slot_of(q, (uint16_t)(i + 1u))];
    }
    q->count--;
    return TESS_OK;
}
