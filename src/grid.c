/* SPDX-License-Identifier: MIT */

#include "tessera/grid.h"

/* Sort key for the fetch order: rings first, then distance within the ring.
 *
 * Chebyshev distance (the larger of |dx| and |dy|) defines the ring, so the
 * whole of the visible middle is requested before anything in the margin.
 * Squared Euclidean distance orders within a ring, which puts the tiles
 * directly above, below and beside the centre ahead of the diagonals -- they
 * are the ones a viewport wider than it is tall actually shows.
 *
 * Both terms are small: at 7x5 the ring is at most 3 and the squared distance
 * at most 13, so packing them into one integer is exact. */
static uint32_t order_key(int dx, int dy)
{
    const int adx = (dx < 0) ? -dx : dx;
    const int ady = (dy < 0) ? -dy : dy;
    const int ring = (adx > ady) ? adx : ady;

    return ((uint32_t)ring << 16) | (uint32_t)(dx * dx + dy * dy);
}

tess_status tess_grid_init(tess_grid *grid, uint8_t cols, uint8_t rows)
{
    if (grid == NULL)
    {
        return TESS_ERR_ARG;
    }
    if (cols == 0u || rows == 0u || cols > TESS_GRID_MAX_COLS || rows > TESS_GRID_MAX_ROWS)
    {
        return TESS_ERR_ARG;
    }
    if ((cols % 2u) == 0u || (rows % 2u) == 0u)
    {
        return TESS_ERR_ARG;
    }

    grid->cols = cols;
    grid->rows = rows;
    grid->count = (uint8_t)(cols * rows);

    const int half_cols = cols / 2;
    const int half_rows = rows / 2;

    /* Insertion sort over at most 35 elements, keyed as above. Insertion sort
     * is stable, so positions with equal keys keep row-major order and the
     * result is fully determined -- the tests check the exact sequence, which
     * they could not do if ties were resolved by the sort's internals. */
    uint32_t keys[TESS_GRID_MAX_TILES];
    uint8_t n = 0;

    for (int row = 0; row < (int)rows; row++)
    {
        for (int col = 0; col < (int)cols; col++)
        {
            const uint32_t key = order_key(col - half_cols, row - half_rows);
            const uint8_t position = (uint8_t)(row * (int)cols + col);

            int i = (int)n - 1;
            while (i >= 0 && keys[i] > key)
            {
                keys[i + 1] = keys[i];
                grid->order[i + 1] = grid->order[i];
                i--;
            }
            keys[i + 1] = key;
            grid->order[i + 1] = position;
            n++;
        }
    }
    return TESS_OK;
}

tess_status tess_grid_init_for_viewport(tess_grid *grid, int32_t width, int32_t height)
{
    if (grid == NULL || width <= 0 || height <= 0)
    {
        return TESS_ERR_ARG;
    }

    /* Tiles needed to span the viewport in the worst alignment (the viewport
     * edge falling one pixel inside a tile), plus one tile of margin on each
     * side, forced odd. */
    const int32_t span_x = (width + TESSERA_TILE_SIZE - 1) / TESSERA_TILE_SIZE + 1;
    const int32_t span_y = (height + TESSERA_TILE_SIZE - 1) / TESSERA_TILE_SIZE + 1;

    int32_t cols = span_x + 2;
    int32_t rows = span_y + 2;

    if ((cols % 2) == 0) { cols++; }
    if ((rows % 2) == 0) { rows++; }
    if (cols > TESS_GRID_MAX_COLS) { cols = TESS_GRID_MAX_COLS; }
    if (rows > TESS_GRID_MAX_ROWS) { rows = TESS_GRID_MAX_ROWS; }

    return tess_grid_init(grid, (uint8_t)cols, (uint8_t)rows);
}

uint8_t tess_grid_count(const tess_grid *grid)
{
    return (grid != NULL) ? grid->count : 0u;
}

tess_status tess_grid_order_at(const tess_grid *grid, uint8_t n, uint8_t *out_col, uint8_t *out_row)
{
    if (grid == NULL || out_col == NULL || out_row == NULL || n >= grid->count)
    {
        return TESS_ERR_ARG;
    }

    const uint8_t position = grid->order[n];
    *out_col = (uint8_t)(position % grid->cols);
    *out_row = (uint8_t)(position / grid->cols);
    return TESS_OK;
}

tess_tile tess_grid_tile_at(const tess_grid *grid, tess_tile centre, uint8_t col, uint8_t row)
{
    tess_tile tile = centre;

    if (grid == NULL || col >= grid->cols || row >= grid->rows)
    {
        /* An address that tess_tile_is_valid() rejects, rather than a plausible
         * one derived from bad input. */
        tile.zoom = -1;
        return tile;
    }

    const int32_t n = tess_tiles_per_axis((int)centre.zoom);

    tile.x = centre.x + (int32_t)col - (int32_t)(grid->cols / 2u);
    tile.y = centre.y + (int32_t)row - (int32_t)(grid->rows / 2u);

    if (n > 0)
    {
        /* C's % keeps the sign of the dividend, so a plain modulo leaves
         * negative x negative. Add n once and take the modulo again: the grid
         * is never wider than the world at the zoom levels this map ships, so
         * a single correction is enough. */
        tile.x %= n;
        if (tile.x < 0)
        {
            tile.x += n;
        }
    }
    return tile;
}
