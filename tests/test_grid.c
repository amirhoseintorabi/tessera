/* SPDX-License-Identifier: MIT */
/*
 * Tests for the tile window and its fetch order.
 *
 * The fetch order is the part a user sees. Tiles arrive from an SD card over
 * tens of milliseconds each, so filling outward from the middle is the
 * difference between a pan that feels immediate and one that does not. The
 * The exact sequence is asserted rather than some property of it, because an
 * order that is subtly wrong -- a transposed row and column, say -- still
 * looks centre-out from a distance.
 */

#include "tessera/grid.h"

#include "check.h"
#include "fixture.h"

static void test_init_requires_an_odd_grid(void)
{
    begin("a grid must have a centre tile");

    tess_grid grid;

    CHECK_STATUS(tess_grid_init(&grid, 5, 3), TESS_OK);
    CHECK_EQ_I(grid.cols, 5);
    CHECK_EQ_I(grid.rows, 3);
    CHECK_EQ_I(tess_grid_count(&grid), 15);

    CHECK_STATUS(tess_grid_init(&grid, 1, 1), TESS_OK);
    CHECK_STATUS(tess_grid_init(&grid, 7, 5), TESS_OK);

    /* Even sizes have no unambiguous middle, and every position derived from
     * "the centre tile" would be half a tile out. */
    CHECK_STATUS(tess_grid_init(&grid, 4, 3), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init(&grid, 5, 2), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init(&grid, 0, 3), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init(&grid, 9, 3), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init(&grid, 5, 7), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init(NULL, 5, 3), TESS_ERR_ARG);
}

static void test_viewport_sizing(void)
{
    begin("the grid derived from a viewport covers it with a tile of margin");

    tess_grid grid;

    /* A 480x272 panel. 480 spans two tiles plus change,
     * so three to cover it and one either side. */
    CHECK_STATUS(tess_grid_init_for_viewport(&grid, 480, 272), TESS_OK);
    CHECK_EQ_I(grid.cols, 5);
    CHECK_EQ_I(grid.rows, 5);
    CHECK(grid.cols * TESSERA_TILE_SIZE >= 480 + 2 * TESSERA_TILE_SIZE);
    CHECK(grid.rows * TESSERA_TILE_SIZE >= 272 + 2 * TESSERA_TILE_SIZE);

    /* Smaller than one tile still gets a 3x3: one visible plus margin. */
    CHECK_STATUS(tess_grid_init_for_viewport(&grid, 100, 100), TESS_OK);
    CHECK_EQ_I(grid.cols, 5);
    CHECK_EQ_I(grid.rows, 5);

    /* Larger than the maxima clamps rather than failing -- showing as much as
     * possible beats refusing to draw. */
    CHECK_STATUS(tess_grid_init_for_viewport(&grid, 4000, 4000), TESS_OK);
    CHECK_EQ_I(grid.cols, TESS_GRID_MAX_COLS);
    CHECK_EQ_I(grid.rows, TESS_GRID_MAX_ROWS);

    CHECK_STATUS(tess_grid_init_for_viewport(&grid, 0, 100), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_init_for_viewport(&grid, 100, -1), TESS_ERR_ARG);
}

static void test_fetch_order_is_centre_first(void)
{
    begin("the centre tile is fetched first, then outward in rings");

    tess_grid grid;
    CHECK_STATUS(tess_grid_init(&grid, 5, 3), TESS_OK);

    uint8_t col = 0, row = 0;
    CHECK_STATUS(tess_grid_order_at(&grid, 0, &col, &row), TESS_OK);
    CHECK_EQ_I(col, 2);
    CHECK_EQ_I(row, 1);

    /* Every position appears exactly once. Building the order by appending and
     * rescanning for duplicates is the obvious approach and is easy to get
     * subtly wrong; asserting the permutation property costs nothing. */
    int seen[TESS_GRID_MAX_TILES] = {0};
    for (uint8_t n = 0; n < tess_grid_count(&grid); n++)
    {
        CHECK_STATUS(tess_grid_order_at(&grid, n, &col, &row), TESS_OK);
        CHECK(col < grid.cols);
        CHECK(row < grid.rows);
        seen[row * grid.cols + col]++;
    }
    for (int i = 0; i < 15; i++)
    {
        CHECK_EQ_I(seen[i], 1);
    }

    CHECK_STATUS(tess_grid_order_at(&grid, 15, &col, &row), TESS_ERR_ARG);
    CHECK_STATUS(tess_grid_order_at(&grid, 0, NULL, &row), TESS_ERR_ARG);
}

static void test_fetch_order_rings_never_go_backwards(void)
{
    begin("no tile is fetched before a tile closer to the centre");

    tess_grid grid;
    CHECK_STATUS(tess_grid_init(&grid, 7, 5), TESS_OK);

    int previous_ring = -1;
    for (uint8_t n = 0; n < tess_grid_count(&grid); n++)
    {
        uint8_t col = 0, row = 0;
        tess_grid_order_at(&grid, n, &col, &row);

        const int dx = (int) col - (int) (grid.cols / 2);
        const int dy = (int) row - (int) (grid.rows / 2);
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;
        const int ring = adx > ady ? adx : ady;

        CHECK(ring >= previous_ring);
        previous_ring = ring;
    }
    CHECK_EQ_I(previous_ring, 3);  /* the far corners of a 7-wide grid */
}

static void test_fetch_order_exact_sequence(void)
{
    begin("the 3x3 fetch order is exactly this");

    /* Written out in full: "roughly centre-out" is satisfied by several
     * orders, and only one of them is the documented one. */
    tess_grid grid;
    CHECK_STATUS(tess_grid_init(&grid, 3, 3), TESS_OK);

    const uint8_t expect_col[] = {1, 1, 0, 2, 1, 0, 2, 0, 2};
    const uint8_t expect_row[] = {1, 0, 1, 1, 2, 0, 0, 2, 2};

    for (uint8_t n = 0; n < 9; n++)
    {
        uint8_t col = 0, row = 0;
        tess_grid_order_at(&grid, n, &col, &row);
        CHECK_EQ_I(col, expect_col[n]);
        CHECK_EQ_I(row, expect_row[n]);
    }
}

static void test_tile_addresses(void)
{
    begin("grid positions map to the tiles around the centre");

    tess_grid grid;
    tess_grid_init(&grid, 5, 3);

    const tess_tile centre = T(1000, 500, 12);

    /* Middle of the grid is the centre tile itself. */
    tess_tile t = tess_grid_tile_at(&grid, centre, 2, 1);
    CHECK_EQ_I(t.x, 1000);
    CHECK_EQ_I(t.y, 500);
    CHECK_EQ_I(t.zoom, 12);

    /* North-west corner is two west and one north. */
    t = tess_grid_tile_at(&grid, centre, 0, 0);
    CHECK_EQ_I(t.x, 998);
    CHECK_EQ_I(t.y, 499);

    /* South-east corner is two east and one south. */
    t = tess_grid_tile_at(&grid, centre, 4, 2);
    CHECK_EQ_I(t.x, 1002);
    CHECK_EQ_I(t.y, 501);

    /* Out-of-range positions produce an address that is not valid, rather than
     * a plausible one derived from garbage. */
    CHECK(!tess_tile_is_valid(tess_grid_tile_at(&grid, centre, 5, 0)));
    CHECK(!tess_tile_is_valid(tess_grid_tile_at(&grid, centre, 0, 3)));
    CHECK(!tess_tile_is_valid(tess_grid_tile_at(NULL, centre, 0, 0)));
}

static void test_x_wraps_at_the_antimeridian(void)
{
    begin("panning across the antimeridian wraps rather than going negative");

    tess_grid grid;
    tess_grid_init(&grid, 5, 3);

    const int32_t n = tess_tiles_per_axis(12);  /* 4096 */

    /* Centre on the westmost tile column. The two grid columns to its west are
     * off the west edge of the world and belong at the east edge. Without the
     * wrap they come out as x = -1 and -2, and the tile source is asked for a
     * file that cannot exist. */
    const tess_tile west_edge = T(0, 500, 12);

    tess_tile t = tess_grid_tile_at(&grid, west_edge, 0, 1);
    CHECK_EQ_I(t.x, n - 2);
    CHECK(tess_tile_is_valid(t));

    t = tess_grid_tile_at(&grid, west_edge, 1, 1);
    CHECK_EQ_I(t.x, n - 1);
    CHECK(tess_tile_is_valid(t));

    /* And the same at the east edge. */
    const tess_tile east_edge = T(n - 1, 500, 12);
    t = tess_grid_tile_at(&grid, east_edge, 3, 1);
    CHECK_EQ_I(t.x, 0);
    CHECK(tess_tile_is_valid(t));
    t = tess_grid_tile_at(&grid, east_edge, 4, 1);
    CHECK_EQ_I(t.x, 1);
    CHECK(tess_tile_is_valid(t));
}

static void test_y_does_not_wrap(void)
{
    begin("there is no tile above the north edge, and it is not faked");

    tess_grid grid;
    tess_grid_init(&grid, 5, 3);

    /* Wrapping y would put an Antarctic tile above the Arctic, which is worse
     * than a blank: it is a plausible-looking image in the wrong place. */
    const tess_tile north = T(1000, 0, 12);
    tess_tile t = tess_grid_tile_at(&grid, north, 2, 0);
    CHECK_EQ_I(t.y, -1);
    CHECK(!tess_tile_is_valid(t));

    const int32_t n = tess_tiles_per_axis(12);
    const tess_tile south = T(1000, n - 1, 12);
    t = tess_grid_tile_at(&grid, south, 2, 2);
    CHECK_EQ_I(t.y, n);
    CHECK(!tess_tile_is_valid(t));
}

static void test_every_grid_size_is_a_permutation(void)
{
    begin("every allowed grid size produces a complete, duplicate-free order");

    for (uint8_t cols = 1; cols <= TESS_GRID_MAX_COLS; cols += 2)
    {
        for (uint8_t rows = 1; rows <= TESS_GRID_MAX_ROWS; rows += 2)
        {
            tess_grid grid;
            CHECK_STATUS(tess_grid_init(&grid, cols, rows), TESS_OK);
            CHECK_EQ_I(tess_grid_count(&grid), cols * rows);

            int seen[TESS_GRID_MAX_TILES] = {0};
            for (uint8_t n = 0; n < tess_grid_count(&grid); n++)
            {
                uint8_t col = 0, row = 0;
                CHECK_STATUS(tess_grid_order_at(&grid, n, &col, &row), TESS_OK);
                seen[row * cols + col]++;
            }
            for (int i = 0; i < cols * rows; i++)
            {
                CHECK_EQ_I(seen[i], 1);
            }
        }
    }
}

int main(void)
{
    printf("tessera: tile window and fetch order\n");

    test_init_requires_an_odd_grid();
    test_viewport_sizing();
    test_fetch_order_is_centre_first();
    test_fetch_order_rings_never_go_backwards();
    test_fetch_order_exact_sequence();
    test_tile_addresses();
    test_x_wraps_at_the_antimeridian();
    test_y_does_not_wrap();
    test_every_grid_size_is_a_permutation();

    REPORT("grid");
}
