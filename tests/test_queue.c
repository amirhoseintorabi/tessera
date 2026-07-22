/* SPDX-License-Identifier: MIT */
/*
 * Tests for the tile request queue.
 *
 * Two of them are worth pointing at, because the property they pin down is one
 * a search function can fail to have while looking entirely correct:
 * `contains_reports_misses` and `contains_only_writes_output_on_a_hit`. A
 * search that reports a hit unconditionally, or that leaves its output
 * parameter untouched on a miss, hands the caller an uninitialised index -- and
 * the caller will use it as an array subscript.
 */

#include "tessera/queue.h"

#include "check.h"
#include "fixture.h"

static void test_empty(void)
{
    begin("a fresh queue is empty and stays consistent");

    tess_queue q;
    tess_queue_init(&q);

    CHECK(tess_queue_is_empty(&q));
    CHECK(!tess_queue_is_full(&q));
    CHECK_EQ_I(tess_queue_count(&q), 0);

    tess_tile out = T(9, 9, 9);
    CHECK_STATUS(tess_queue_pop(&q, &out), TESS_ERR_EMPTY);
    /* An empty pop must not write the output: a caller that checks the status
     * is fine either way, but one that does not should see its own value
     * rather than a plausible-looking tile address. */
    CHECK_EQ_I(out.x, 9);
    CHECK_STATUS(tess_queue_peek(&q, &out), TESS_ERR_EMPTY);
}

static void test_fifo_order(void)
{
    begin("tiles come back in the order they went in");

    tess_queue q;
    tess_queue_init(&q);

    for (int32_t i = 0; i < 10; i++)
    {
        CHECK_STATUS(tess_queue_push(&q, T(i, 100, 12)), TESS_OK);
    }
    CHECK_EQ_I(tess_queue_count(&q), 10);

    for (int32_t i = 0; i < 10; i++)
    {
        tess_tile out = T(-1, -1, -1);
        CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
        CHECK_EQ_I(out.x, i);
        CHECK_EQ_I(out.y, 100);
        CHECK_EQ_I(out.zoom, 12);
    }
    CHECK(tess_queue_is_empty(&q));
}

static void test_dedup(void)
{
    begin("pushing a tile that is already queued does not queue it twice");

    tess_queue q;
    tess_queue_init(&q);

    CHECK_STATUS(tess_queue_push(&q, T(1, 2, 12)), TESS_OK);
    CHECK_STATUS(tess_queue_push(&q, T(1, 2, 12)), TESS_OK);
    CHECK_STATUS(tess_queue_push(&q, T(1, 2, 12)), TESS_OK);
    CHECK_EQ_I(tess_queue_count(&q), 1);

    /* Same x and y, different zoom, is a different tile. A cache or queue that
     * ignored zoom would serve the wrong image after every zoom change. */
    CHECK_STATUS(tess_queue_push(&q, T(1, 2, 13)), TESS_OK);
    CHECK_EQ_I(tess_queue_count(&q), 2);

    /* Deduplication must not reorder: the first request keeps its place. */
    tess_tile out;
    CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
    CHECK_EQ_I(out.zoom, 12);
}

static void test_contains_reports_misses(void)
{
    begin("contains() reports a miss as a miss");

    tess_queue q;
    tess_queue_init(&q);
    tess_queue_push(&q, T(1, 1, 12));
    tess_queue_push(&q, T(2, 2, 12));
    tess_queue_push(&q, T(3, 3, 12));

    CHECK(!tess_queue_contains(&q, T(4, 4, 12), NULL));
    CHECK(!tess_queue_contains(&q, T(1, 1, 13), NULL));
    CHECK(tess_queue_contains(&q, T(3, 3, 12), NULL));

    /* And it searches the whole queue, not just the first entry. */
    uint16_t position = 0xFFFFu;
    CHECK(tess_queue_contains(&q, T(3, 3, 12), &position));
    CHECK_EQ_I(position, 2);
}

static void test_contains_only_writes_output_on_a_hit(void)
{
    begin("contains() leaves its output alone when it misses");

    tess_queue q;
    tess_queue_init(&q);
    tess_queue_push(&q, T(1, 1, 12));

    uint16_t position = 4242u;
    CHECK(!tess_queue_contains(&q, T(7, 7, 12), &position));
    CHECK_EQ_I(position, 4242);
}

static void test_remove(void)
{
    begin("remove() takes out one entry and keeps the rest in order");

    tess_queue q;
    tess_queue_init(&q);
    for (int32_t i = 0; i < 5; i++)
    {
        tess_queue_push(&q, T(i, 0, 12));
    }

    CHECK_STATUS(tess_queue_remove(&q, T(2, 0, 12)), TESS_OK);
    CHECK_EQ_I(tess_queue_count(&q), 4);
    CHECK(!tess_queue_contains(&q, T(2, 0, 12), NULL));

    CHECK_STATUS(tess_queue_remove(&q, T(2, 0, 12)), TESS_ERR_NOT_FOUND);

    const int32_t expect[] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++)
    {
        tess_tile out;
        CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
        CHECK_EQ_I(out.x, expect[i]);
    }
}

static void test_remove_head_and_tail(void)
{
    begin("remove() handles the head and the tail");

    tess_queue q;
    tess_queue_init(&q);
    for (int32_t i = 0; i < 3; i++)
    {
        tess_queue_push(&q, T(i, 0, 12));
    }

    CHECK_STATUS(tess_queue_remove(&q, T(0, 0, 12)), TESS_OK);
    CHECK_STATUS(tess_queue_remove(&q, T(2, 0, 12)), TESS_OK);
    CHECK_EQ_I(tess_queue_count(&q), 1);

    tess_tile out;
    CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
    CHECK_EQ_I(out.x, 1);
    CHECK(tess_queue_is_empty(&q));
}

static void test_full(void)
{
    begin("a full queue refuses new work rather than overwriting old");

    tess_queue q;
    tess_queue_init(&q);

    for (int32_t i = 0; i < TESS_QUEUE_CAPACITY; i++)
    {
        CHECK_STATUS(tess_queue_push(&q, T(i, 0, 12)), TESS_OK);
    }
    CHECK(tess_queue_is_full(&q));
    CHECK_STATUS(tess_queue_push(&q, T(999, 0, 12)), TESS_ERR_FULL);

    /* The rejected push must not have disturbed anything. */
    CHECK_EQ_I(tess_queue_count(&q), TESS_QUEUE_CAPACITY);
    tess_tile out;
    CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
    CHECK_EQ_I(out.x, 0);
}

static void test_wraps(void)
{
    begin("the ring survives many more pushes than it has slots");

    tess_queue q;
    tess_queue_init(&q);

    /* Push and pop 10x the capacity, keeping the queue part-full throughout, so
     * that head and the write position each wrap several times and at
     * different moments. */
    int32_t next_in = 0;
    int32_t next_out = 0;

    for (int round = 0; round < TESS_QUEUE_CAPACITY * 10; round++)
    {
        if (tess_queue_count(&q) < 5)
        {
            CHECK_STATUS(tess_queue_push(&q, T(next_in++, 0, 12)), TESS_OK);
            CHECK_STATUS(tess_queue_push(&q, T(next_in++, 0, 12)), TESS_OK);
        }

        tess_tile out;
        CHECK_STATUS(tess_queue_pop(&q, &out), TESS_OK);
        CHECK_EQ_I(out.x, next_out++);
    }
}

static void test_clear(void)
{
    begin("clear() makes everything unreachable");

    tess_queue q;
    tess_queue_init(&q);
    for (int32_t i = 0; i < 20; i++)
    {
        tess_queue_push(&q, T(i, 0, 12));
    }
    /* Pop a few first, so head is not at 0 when it is cleared -- a clear that
     * only reset the count would leave stale entries visible. */
    tess_tile out;
    tess_queue_pop(&q, &out);
    tess_queue_pop(&q, &out);

    tess_queue_clear(&q);

    CHECK(tess_queue_is_empty(&q));
    CHECK_EQ_I(tess_queue_count(&q), 0);
    for (int32_t i = 0; i < 20; i++)
    {
        CHECK(!tess_queue_contains(&q, T(i, 0, 12), NULL));
    }
    CHECK_STATUS(tess_queue_pop(&q, &out), TESS_ERR_EMPTY);
}

static void test_rejects_impossible_tiles(void)
{
    begin("a tile address that cannot exist is never queued");

    tess_queue q;
    tess_queue_init(&q);

    /* A negative row is what an unclamped latitude produces, and it is the
     * value that goes on to be formatted into a path. */
    CHECK_STATUS(tess_queue_push(&q, T(100, -1, 12)), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_push(&q, T(100, -8166, 12)), TESS_ERR_ARG);
    /* Past the east edge at this zoom (2^12 = 4096 tiles per axis). */
    CHECK_STATUS(tess_queue_push(&q, T(4096, 100, 12)), TESS_ERR_ARG);
    /* A zoom outside the configured range. */
    CHECK_STATUS(tess_queue_push(&q, T(1, 1, TESSERA_MAX_ZOOM + 1)), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_push(&q, T(1, 1, -1)), TESS_ERR_ARG);

    CHECK(tess_queue_is_empty(&q));
}

static void test_null_safety(void)
{
    begin("NULL arguments are refused, not dereferenced");

    tess_tile out;
    tess_queue q;
    tess_queue_init(&q);

    tess_queue_init(NULL);
    tess_queue_clear(NULL);
    CHECK_EQ_I(tess_queue_count(NULL), 0);
    CHECK(tess_queue_is_empty(NULL));
    CHECK(!tess_queue_contains(NULL, T(1, 1, 12), NULL));
    CHECK_STATUS(tess_queue_push(NULL, T(1, 1, 12)), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_pop(NULL, &out), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_pop(&q, NULL), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_peek(&q, NULL), TESS_ERR_ARG);
    CHECK_STATUS(tess_queue_remove(NULL, T(1, 1, 12)), TESS_ERR_ARG);
}

int main(void)
{
    printf("tessera: tile request queue\n");

    test_empty();
    test_fifo_order();
    test_dedup();
    test_contains_reports_misses();
    test_contains_only_writes_output_on_a_hit();
    test_remove();
    test_remove_head_and_tail();
    test_full();
    test_wraps();
    test_clear();
    test_rejects_impossible_tiles();
    test_null_safety();

    REPORT("queue");
}
