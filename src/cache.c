/* SPDX-License-Identifier: MIT */

#include "tessera/cache.h"

static bool index_is_valid(const tess_cache *cache, int index)
{
    return cache != NULL && index >= 0 && index < (int)cache->count;
}

tess_status tess_cache_init(tess_cache *cache, tess_slot *slots, uint16_t count)
{
    if (cache == NULL || slots == NULL || count == 0u)
    {
        return TESS_ERR_ARG;
    }

    for (uint16_t i = 0; i < count; i++)
    {
        if (slots[i].image == NULL)
        {
            return TESS_ERR_ARG;
        }
    }

    cache->slots = slots;
    cache->count = count;
    cache->frame = 1u;  /* not 0, so `used_at == 0` reads as "never touched" */

    for (uint16_t i = 0; i < count; i++)
    {
        slots[i].state = TESS_SLOT_FREE;
        slots[i].used_at = 0u;
        slots[i].tile.x = 0;
        slots[i].tile.y = 0;
        slots[i].tile.zoom = 0;
    }
    return TESS_OK;
}

void tess_cache_begin_frame(tess_cache *cache)
{
    if (cache == NULL)
    {
        return;
    }

    /* Wrapping after 2^32 frames would make one round of eviction pick the
     * wrong victim, which costs a single redundant tile read and nothing else.
     * At the widget's 10 Hz refresh that is 13 years away, and handling it by
     * rebasing every slot's stamp would cost more than the defect. */
    cache->frame++;
}

int tess_cache_find(const tess_cache *cache, tess_tile tile)
{
    if (cache == NULL)
    {
        return -1;
    }

    for (uint16_t i = 0; i < cache->count; i++)
    {
        if (cache->slots[i].state == TESS_SLOT_READY && tess_tile_equal(cache->slots[i].tile, tile))
        {
            return (int)i;
        }
    }
    return -1;
}

int tess_cache_find_pending(const tess_cache *cache, tess_tile tile)
{
    if (cache == NULL)
    {
        return -1;
    }

    for (uint16_t i = 0; i < cache->count; i++)
    {
        const tess_slot *slot = &cache->slots[i];
        if (slot->state != TESS_SLOT_FREE && tess_tile_equal(slot->tile, tile))
        {
            return (int)i;
        }
    }
    return -1;
}

void tess_cache_touch(tess_cache *cache, int index)
{
    if (!index_is_valid(cache, index))
    {
        return;
    }
    cache->slots[index].used_at = cache->frame;
}

tess_status tess_cache_acquire(tess_cache *cache, tess_tile tile, int *out_index)
{
    if (cache == NULL || out_index == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (!tess_tile_is_valid(tile))
    {
        return TESS_ERR_ARG;
    }

    /* Already here, or already on its way. Returning the existing slot rather
     * than starting a second read is what keeps a slow medium from starving
     * the queue with duplicate work. */
    const int existing = tess_cache_find_pending(cache, tile);
    if (existing >= 0)
    {
        *out_index = existing;
        return TESS_OK;
    }

    int victim = -1;
    uint32_t oldest = 0;

    for (uint16_t i = 0; i < cache->count; i++)
    {
        tess_slot *slot = &cache->slots[i];

        if (slot->state == TESS_SLOT_FREE)
        {
            victim = (int)i;  /* a free slot always wins; stop looking */
            break;
        }
        if (slot->state != TESS_SLOT_READY)
        {
            continue;  /* LOADING: someone is writing into it */
        }
        if (victim < 0 || slot->used_at < oldest)
        {
            victim = (int)i;
            oldest = slot->used_at;
        }
    }

    if (victim < 0)
    {
        return TESS_ERR_FULL;
    }

    cache->slots[victim].state = TESS_SLOT_LOADING;
    cache->slots[victim].tile = tile;
    cache->slots[victim].used_at = cache->frame;
    *out_index = victim;
    return TESS_OK;
}

tess_status tess_cache_commit(tess_cache *cache, int index)
{
    if (!index_is_valid(cache, index))
    {
        return TESS_ERR_ARG;
    }
    /* Only a reservation can be committed. Committing a FREE slot would
     * publish an image nobody wrote, and committing a READY one would mean two
     * loaders shared a reservation. */
    if (cache->slots[index].state != TESS_SLOT_LOADING)
    {
        return TESS_ERR_ARG;
    }

    cache->slots[index].state = TESS_SLOT_READY;
    cache->slots[index].used_at = cache->frame;
    return TESS_OK;
}

tess_status tess_cache_release(tess_cache *cache, int index)
{
    if (!index_is_valid(cache, index))
    {
        return TESS_ERR_ARG;
    }
    if (cache->slots[index].state != TESS_SLOT_LOADING)
    {
        return TESS_ERR_ARG;
    }

    cache->slots[index].state = TESS_SLOT_FREE;
    cache->slots[index].used_at = 0u;
    return TESS_OK;
}

void tess_cache_reset(tess_cache *cache)
{
    if (cache == NULL)
    {
        return;
    }

    for (uint16_t i = 0; i < cache->count; i++)
    {
        cache->slots[i].state = TESS_SLOT_FREE;
        cache->slots[i].used_at = 0u;
    }
}

void tess_cache_stats(const tess_cache *cache, uint16_t *out_free, uint16_t *out_loading,
                    uint16_t *out_ready)
{
    uint16_t counts[3] = {0, 0, 0};

    if (cache != NULL)
    {
        for (uint16_t i = 0; i < cache->count; i++)
        {
            counts[cache->slots[i].state]++;
        }
    }

    if (out_free != NULL)    { *out_free = counts[TESS_SLOT_FREE]; }
    if (out_loading != NULL) { *out_loading = counts[TESS_SLOT_LOADING]; }
    if (out_ready != NULL)   { *out_ready = counts[TESS_SLOT_READY]; }
}
