/* SPDX-License-Identifier: MIT */
/*
 * Tests for the tile slot cache.
 *
 * Three of them carry most of the weight, because each pins down a property
 * that is invisible when it holds and disastrous when it does not:
 *
 *   - a LOADING slot is never evicted, so a reservation cannot be pulled out
 *     from under the reader while it writes;
 *   - a slot is only drawable after commit(), so a half-decoded image cannot
 *     be found by tess_cache_find();
 *   - slots really are recycled, so a long pan does not run the cache dry.
 */

#include "tessera/cache.h"

#include "check.h"

#define SLOTS 4

static uint8_t buffers[SLOTS][8];
static tess_slot slots[SLOTS];

static tess_tile T(int32_t x, int32_t y, int32_t zoom)
{
    tess_tile t;
    t.x = x;
    t.y = y;
    t.zoom = zoom;
    return t;
}

static void reset(tess_cache *cache)
{
    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = buffers[i];
    }
    CHECK_STATUS(tess_cache_init(cache, slots, SLOTS), TESS_OK);
}

/* Acquire, then immediately commit -- the successful-load sequence. */
static int load(tess_cache *cache, tess_tile tile)
{
    int index = -1;
    if (tess_cache_acquire(cache, tile, &index) != TESS_OK)
    {
        return -1;
    }
    tess_cache_commit(cache, index);
    return index;
}

static void test_init(void)
{
    begin("init requires somewhere to decode into");

    tess_cache cache;
    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = buffers[i];
    }

    CHECK_STATUS(tess_cache_init(NULL, slots, SLOTS), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_init(&cache, NULL, SLOTS), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_init(&cache, slots, 0), TESS_ERR_ARG);

    /* A slot with no buffer is a wiring mistake worth catching at start-up
     * rather than as a null write during the first pan. */
    slots[2].image = NULL;
    CHECK_STATUS(tess_cache_init(&cache, slots, SLOTS), TESS_ERR_ARG);
    slots[2].image = buffers[2];

    CHECK_STATUS(tess_cache_init(&cache, slots, SLOTS), TESS_OK);

    uint16_t free_count = 0, loading = 0, ready = 0;
    tess_cache_stats(&cache, &free_count, &loading, &ready);
    CHECK_EQ_I(free_count, SLOTS);
    CHECK_EQ_I(loading, 0);
    CHECK_EQ_I(ready, 0);
}

static void test_find_and_commit(void)
{
    begin("a slot is only findable once its image is complete");

    tess_cache cache;
    reset(&cache);

    const tess_tile tile = T(10, 20, 12);

    int index = -1;
    CHECK_STATUS(tess_cache_acquire(&cache, tile, &index), TESS_OK);
    CHECK(index >= 0);

    /* Reserved but not yet written: the drawing side must not find it. The
     * window in which a slot holds undefined bytes is exactly as wide as the
     * decode, and it is on the other thread. */
    CHECK_EQ_I(tess_cache_find(&cache, tile), -1);
    CHECK_EQ_I(tess_cache_find_pending(&cache, tile), index);

    CHECK_STATUS(tess_cache_commit(&cache, index), TESS_OK);
    CHECK_EQ_I(tess_cache_find(&cache, tile), index);
    CHECK(cache.slots[index].image == buffers[index]);
}

static void test_commit_and_release_are_guarded(void)
{
    begin("commit and release only apply to a live reservation");

    tess_cache cache;
    reset(&cache);

    CHECK_STATUS(tess_cache_commit(&cache, 0), TESS_ERR_ARG);   /* FREE  */
    CHECK_STATUS(tess_cache_release(&cache, 0), TESS_ERR_ARG);  /* FREE  */
    CHECK_STATUS(tess_cache_commit(&cache, -1), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_commit(&cache, SLOTS), TESS_ERR_ARG);

    int index = -1;
    tess_cache_acquire(&cache, T(1, 1, 12), &index);
    CHECK_STATUS(tess_cache_commit(&cache, index), TESS_OK);
    /* Committing twice would mean two loaders shared one reservation. */
    CHECK_STATUS(tess_cache_commit(&cache, index), TESS_ERR_ARG);
}

static void test_release_frees_the_slot(void)
{
    begin("a failed load gives the slot back and leaves nothing drawable");

    tess_cache cache;
    reset(&cache);

    const tess_tile tile = T(5, 5, 12);
    int index = -1;
    tess_cache_acquire(&cache, tile, &index);
    CHECK_STATUS(tess_cache_release(&cache, index), TESS_OK);

    CHECK_EQ_I(tess_cache_find(&cache, tile), -1);
    CHECK_EQ_I(tess_cache_find_pending(&cache, tile), -1);

    uint16_t free_count = 0;
    tess_cache_stats(&cache, &free_count, NULL, NULL);
    CHECK_EQ_I(free_count, SLOTS);
}

static void test_acquire_is_idempotent(void)
{
    begin("acquiring a tile that is already here returns the same slot");

    tess_cache cache;
    reset(&cache);

    const tess_tile tile = T(3, 4, 12);

    int first = -1;
    CHECK_STATUS(tess_cache_acquire(&cache, tile, &first), TESS_OK);

    int again = -1;
    CHECK_STATUS(tess_cache_acquire(&cache, tile, &again), TESS_OK);
    CHECK_EQ_I(again, first);

    tess_cache_commit(&cache, first);

    int third = -1;
    CHECK_STATUS(tess_cache_acquire(&cache, tile, &third), TESS_OK);
    CHECK_EQ_I(third, first);
    /* And the ready image was not knocked back into LOADING by the request. */
    CHECK_EQ_I(tess_cache_find(&cache, tile), first);
}

static void test_slots_are_recycled(void)
{
    begin("slots are reused, so a long pan does not run the cache dry");

    tess_cache cache;
    reset(&cache);

    /* Load many more tiles than there are slots. If slots are not returned to
     * the pool this stops working at the fourth one, and a map that has been
     * panned far enough simply stops loading. */
    for (int32_t i = 0; i < 100; i++)
    {
        tess_cache_begin_frame(&cache);
        const int index = load(&cache, T(i, 0, 12));
        CHECK(index >= 0);
        CHECK_EQ_I(tess_cache_find(&cache, T(i, 0, 12)), index);
    }

    uint16_t ready = 0, loading = 0;
    tess_cache_stats(&cache, NULL, &loading, &ready);
    CHECK_EQ_I(ready, SLOTS);
    CHECK_EQ_I(loading, 0);
}

static void test_lru_evicts_the_least_recently_wanted(void)
{
    begin("eviction takes the tile the view stopped wanting first");

    tess_cache cache;
    reset(&cache);

    const tess_tile a = T(1, 0, 12);
    const tess_tile b = T(2, 0, 12);
    const tess_tile c = T(3, 0, 12);
    const tess_tile d = T(4, 0, 12);
    const tess_tile e = T(5, 0, 12);

    tess_cache_begin_frame(&cache);
    load(&cache, a);
    load(&cache, b);
    load(&cache, c);
    load(&cache, d);

    /* A new frame in which everything except `a` is still wanted. */
    tess_cache_begin_frame(&cache);
    tess_cache_touch(&cache, tess_cache_find(&cache, b));
    tess_cache_touch(&cache, tess_cache_find(&cache, c));
    tess_cache_touch(&cache, tess_cache_find(&cache, d));

    /* So the fifth tile must displace `a` and nothing else. */
    load(&cache, e);

    CHECK_EQ_I(tess_cache_find(&cache, a), -1);
    CHECK(tess_cache_find(&cache, b) >= 0);
    CHECK(tess_cache_find(&cache, c) >= 0);
    CHECK(tess_cache_find(&cache, d) >= 0);
    CHECK(tess_cache_find(&cache, e) >= 0);
}

static void test_loading_slots_are_never_evicted(void)
{
    begin("a slot being written into is not handed to someone else");

    tess_cache cache;
    reset(&cache);

    /* Reserve every slot without committing any: four loads in flight. */
    int reserved[SLOTS];
    for (int i = 0; i < SLOTS; i++)
    {
        CHECK_STATUS(tess_cache_acquire(&cache, T(i, 0, 12), &reserved[i]), TESS_OK);
    }

    /* There is nowhere to put a fifth. TESS_ERR_FULL is the honest answer; the
     * alternative -- evicting a reservation -- would have two loaders writing
     * the same buffer, and the drawing side would eventually commit whichever
     * finished second under the first one's tile address. */
    int index = -1;
    CHECK_STATUS(tess_cache_acquire(&cache, T(99, 0, 12), &index), TESS_ERR_FULL);

    uint16_t loading = 0;
    tess_cache_stats(&cache, NULL, &loading, NULL);
    CHECK_EQ_I(loading, SLOTS);

    /* Once one finishes, the fifth fits. */
    tess_cache_commit(&cache, reserved[0]);
    tess_cache_begin_frame(&cache);
    CHECK_STATUS(tess_cache_acquire(&cache, T(99, 0, 12), &index), TESS_OK);
}

static void test_zoom_is_part_of_the_address(void)
{
    begin("the same x,y at another zoom is a different tile");

    tess_cache cache;
    reset(&cache);

    load(&cache, T(100, 200, 12));

    CHECK(tess_cache_find(&cache, T(100, 200, 12)) >= 0);
    CHECK_EQ_I(tess_cache_find(&cache, T(100, 200, 13)), -1);
    CHECK_EQ_I(tess_cache_find(&cache, T(100, 200, 11)), -1);
}

static void test_find_does_not_reorder(void)
{
    begin("looking a tile up does not by itself protect it from eviction");

    tess_cache cache;
    reset(&cache);

    const tess_tile a = T(1, 0, 12);

    tess_cache_begin_frame(&cache);
    load(&cache, a);
    load(&cache, T(2, 0, 12));
    load(&cache, T(3, 0, 12));
    load(&cache, T(4, 0, 12));

    tess_cache_begin_frame(&cache);
    /* Deciding not to draw `a` this frame is exactly the case where a find()
     * that also touched would keep the wrong tile alive. */
    CHECK(tess_cache_find(&cache, a) >= 0);
    tess_cache_touch(&cache, tess_cache_find(&cache, T(2, 0, 12)));
    tess_cache_touch(&cache, tess_cache_find(&cache, T(3, 0, 12)));
    tess_cache_touch(&cache, tess_cache_find(&cache, T(4, 0, 12)));

    load(&cache, T(5, 0, 12));
    CHECK_EQ_I(tess_cache_find(&cache, a), -1);
}

static void test_acquire_rejects_impossible_tiles(void)
{
    begin("an impossible tile address is never given a slot");

    tess_cache cache;
    reset(&cache);

    int index = 7;
    CHECK_STATUS(tess_cache_acquire(&cache, T(0, -1, 12), &index), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_acquire(&cache, T(0, 0, 42), &index), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_acquire(&cache, T(1 << 12, 0, 12), &index), TESS_ERR_ARG);

    uint16_t free_count = 0;
    tess_cache_stats(&cache, &free_count, NULL, NULL);
    CHECK_EQ_I(free_count, SLOTS);
}

static void test_null_safety(void)
{
    begin("NULL arguments are refused, not dereferenced");

    int index = 0;
    CHECK_EQ_I(tess_cache_find(NULL, T(1, 1, 12)), -1);
    CHECK_EQ_I(tess_cache_find_pending(NULL, T(1, 1, 12)), -1);
    CHECK_STATUS(tess_cache_acquire(NULL, T(1, 1, 12), &index), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_commit(NULL, 0), TESS_ERR_ARG);
    CHECK_STATUS(tess_cache_release(NULL, 0), TESS_ERR_ARG);
    tess_cache_begin_frame(NULL);
    tess_cache_touch(NULL, 0);
    tess_cache_reset(NULL);
    tess_cache_stats(NULL, NULL, NULL, NULL);

    tess_cache cache;
    reset(&cache);
    /* A failed find returns -1, and passing that straight to touch is the
     * natural call shape -- so it has to be harmless. */
    tess_cache_touch(&cache, tess_cache_find(&cache, T(77, 77, 12)));
}

int main(void)
{
    printf("tessera: tile slot cache\n");

    test_init();
    test_find_and_commit();
    test_commit_and_release_are_guarded();
    test_release_frees_the_slot();
    test_acquire_is_idempotent();
    test_slots_are_recycled();
    test_lru_evicts_the_least_recently_wanted();
    test_loading_slots_are_never_evicted();
    test_zoom_is_part_of_the_address();
    test_find_does_not_reorder();
    test_acquire_rejects_impossible_tiles();
    test_null_safety();

    REPORT("cache");
}
