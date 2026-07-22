/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_TYPES_H
#define TESSERA_TYPES_H

/*
 * Shared vocabulary: status codes, screen geometry, and the tile-address
 * helpers everything else is phrased in.
 *
 * Screen coordinates are pixels with the origin at the top-left of the widget,
 * +x east and +y south. That is deliberately the same convention as the
 * slippy-map tile axes and as almost every framebuffer, so no layer in this
 * library has to flip a sign. Sign flips between layers are where placement
 * bugs live.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tessera/projection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- status codes */

typedef enum
{
    TESS_OK = 0,
    TESS_ERR_ARG,       /* NULL, or a value outside what the call accepts    */
    TESS_ERR_FULL,      /* no room: the queue is full, or every slot is busy */
    TESS_ERR_EMPTY,     /* nothing to take                                   */
    TESS_ERR_NOT_FOUND, /* no such entry, or no such tile on the medium      */
    TESS_ERR_IO,        /* the medium failed, or returned the wrong length   */
    TESS_ERR_RANGE      /* the result would not fit the destination          */
} tess_status;

/* A short name for a status, for logs and test failure messages. */
const char *tess_status_name(tess_status status);

/* ---------------------------------------------------------- screen geometry */

typedef struct
{
    int32_t x;
    int32_t y;
} tess_point;

/*
 * A screen rectangle, half-open: x0 <= x < x1 and y0 <= y < y1.
 *
 * Half-open makes the width x1-x0 rather than x1-x0+1, and lets adjacent
 * rectangles tile the plane without overlapping. It also removes a class of
 * off-by-one: with an inclusive rectangle and a strict `>` containment test, a
 * marker sitting exactly on the left edge reads as outside, and a widget that
 * draws off-screen markers differently then draws it twice.
 */
typedef struct
{
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} tess_rect;

static inline int32_t tess_rect_width(tess_rect r) { return r.x1 - r.x0; }
static inline int32_t tess_rect_height(tess_rect r) { return r.y1 - r.y0; }

static inline bool tess_rect_is_empty(tess_rect r)
{
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

static inline bool tess_rect_contains(tess_rect r, tess_point p)
{
    return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1;
}

/* Shrink a rectangle by `dx` on each vertical edge and `dy` on each horizontal
 * one. Never produces an inverted rectangle: past the point where the edges
 * would cross it collapses to the centre, because "there is no inside left" is
 * an answer callers can handle and a negative width is not. */
tess_rect tess_rect_inset(tess_rect r, int32_t dx, int32_t dy);

/* ------------------------------------------------------------ tile addresses */

/* Tile addresses compare on all three fields. Zoom is part of the identity: the
 * same (x, y) at two zoom levels names two completely different places, and a
 * cache that ignored it would serve the wrong image after every zoom change. */
static inline bool tess_tile_equal(tess_tile a, tess_tile b)
{
    return a.x == b.x && a.y == b.y && a.zoom == b.zoom;
}

/*
 * True if `tile` names a tile that can exist.
 *
 * x is expected to have been wrapped into 0..2^z-1 already -- tess_grid_tile_at
 * does that, so the antimeridian is crossed rather than hit. y is not wrapped,
 * because there is nothing above the north edge of the world to wrap to, so a y
 * outside the range is a genuine "no tile here".
 *
 * Saying so explicitly is what keeps impossible addresses away from the
 * filesystem and away from any string formatting. A GNSS receiver reporting a
 * garbage latitude during cold start is the ordinary way an out-of-range row
 * gets produced.
 */
bool tess_tile_is_valid(tess_tile tile);

/*
 * Longest path tess_tile_path can produce for the default pattern, including
 * the terminator. Sized with headroom for a zoom-22 address; the function
 * bounds itself regardless of what the caller allocated.
 */
#define TESS_TILE_PATH_MAX 64

/* The layout tess_tile_path uses when given no pattern: the z/x/y directory
 * convention that most tile tooling already writes. */
#define TESS_TILE_PATH_DEFAULT "{z}/{x}/{y}.bin"

/*
 * Format the storage path of a tile into `buf` from a pattern.
 *
 * The pattern is copied literally except for three tokens:
 *
 *     {z}   zoom level
 *     {x}   tile column
 *     {y}   tile row
 *
 * so "tiles/{z}/{x}/{y}.bin" gives "tiles/14/8531/5423.bin", and a flat layout
 * such as "map/t_{x}_{y}_{z}.raw" works equally well. NULL selects
 * TESS_TILE_PATH_DEFAULT.
 *
 * Returns TESS_ERR_RANGE rather than truncating -- a truncated path names the
 * wrong file, and possibly one that exists -- and TESS_ERR_ARG for a tile that
 * tess_tile_is_valid rejects, since formatting an impossible address has no
 * useful outcome. This is exactly the place an unbounded sprintf into a fixed
 * buffer goes wrong: every plausible address is short, and the pathological
 * ones are not.
 */
tess_status tess_tile_path(char *buf, size_t buf_size, const char *pattern, tess_tile tile);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_TYPES_H */
