/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_GRID_H
#define TESSERA_GRID_H

/*
 * The window of tiles held around the view centre, and the order they are
 * fetched in.
 *
 * The grid is always an odd number of columns by an odd number of rows, so
 * that there is a single unambiguous centre tile. Its job is two questions:
 *
 *   - which tile address sits at grid position (column, row)?
 *   - in what order should the fifteen-or-so addresses be requested?
 *
 * The second one is what the user sees. Tiles arrive over tens of milliseconds
 * each from an SD card, so on a pan the difference between filling the middle
 * of the screen first and filling the top-left first is the difference between
 * a map that feels responsive and one that does not. The order here is
 * centre-out: rings of increasing Chebyshev distance, and within a ring,
 * nearest first.
 *
 * The order is computed once at init and asserted exactly in the tests. "Kind
 * of centre-out" is satisfied by several different orders, and only one of them
 * puts the four edge-adjacent tiles ahead of the four diagonals -- which is
 * what a viewport wider than it is tall actually shows first.
 */

#include "tessera/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TESS_GRID_MAX_COLS 7
#define TESS_GRID_MAX_ROWS 5
#define TESS_GRID_MAX_TILES (TESS_GRID_MAX_COLS * TESS_GRID_MAX_ROWS)

typedef struct
{
    uint8_t cols;
    uint8_t rows;
    /* Grid positions in fetch order, packed as row * cols + col. */
    uint8_t order[TESS_GRID_MAX_TILES];
    uint8_t count;
} tess_grid;

/*
 * Build a grid of `cols` x `rows`.
 *
 * Both must be odd and within the maxima above; anything else is TESS_ERR_ARG
 * rather than being quietly rounded, because a grid with no centre tile makes
 * every position derived from it ambiguous by half a tile.
 */
tess_status tess_grid_init(tess_grid *grid, uint8_t cols, uint8_t rows);

/*
 * Build the smallest odd grid that covers a `width` x `height` viewport with
 * one tile of margin on each side.
 *
 * The margin is what lets a pan of up to a tile happen without exposing an
 * unloaded edge. Clamped to the maxima, so a viewport larger than 7x5 tiles
 * gets the largest grid available rather than a failure.
 */
tess_status tess_grid_init_for_viewport(tess_grid *grid, int32_t width, int32_t height);

/* Number of tiles in the grid, i.e. cols * rows. */
uint8_t tess_grid_count(const tess_grid *grid);

/*
 * The grid position `n` steps into the fetch order, with 0 being the centre
 * tile. Returns TESS_ERR_ARG if `n` is past the end.
 */
tess_status tess_grid_order_at(const tess_grid *grid, uint8_t n, uint8_t *out_col, uint8_t *out_row);

/*
 * The tile address at grid position (col, row) given the centre tile.
 *
 * x is wrapped modulo 2^zoom, so panning across the antimeridian continues
 * onto the far side of the world instead of producing a negative column. y is
 * *not* wrapped -- there is no tile above the north edge to wrap onto, and
 * wrapping would put an antarctic tile above an arctic one, which is worse
 * than a blank because it looks like data. Positions off the top or bottom
 * come back as an address tess_tile_is_valid() rejects, and the caller draws a
 * blank rather than issuing a read.
 */
tess_tile tess_grid_tile_at(const tess_grid *grid, tess_tile centre, uint8_t col, uint8_t row);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_GRID_H */
