/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PROJECTION_H
#define TESSERA_PROJECTION_H

/*
 * Web Mercator (EPSG:3857) slippy-map tile arithmetic.
 *
 * The coordinate layer, kept free of any display, filesystem or RTOS
 * dependency so that it can be compiled and checked on a workstation against
 * published reference values. It needs nothing beyond <math.h>.
 *
 * Conventions, which are the standard slippy-map ones:
 *
 *   - Tiles are 256x256 pixels.
 *   - At zoom z there are 2^z tiles on each axis.
 *   - Tile (0,0) is the north-west corner of the world; x increases east,
 *     y increases *south*.
 *   - Latitude is clamped to +/-85.0511287798 degrees, the point at which the
 *     Mercator projection reaches a square world. Feeding it +/-90 produces an
 *     infinity, so the clamp is not optional.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TESSERA_TILE_SIZE 256

/* The latitude beyond which Web Mercator is not defined for a square world. */
#define TESSERA_MAX_LATITUDE 85.05112877980659

/*
 * The zoom range this library will produce addresses for.
 *
 * 0 is the whole world in one tile and 22 is roughly 3 cm per pixel, which
 * covers what raster tile sets are usually cut to. Narrow it at build time to
 * match the tiles actually shipped on a device -- a view that can never reach
 * a zoom it has no tiles for is one fewer blank screen to explain:
 *
 *     -DTESSERA_MIN_ZOOM=10 -DTESSERA_MAX_ZOOM=16
 *
 * The upper bound cannot exceed 30: 2^31 tiles per axis overflows int32_t.
 */
#ifndef TESSERA_MIN_ZOOM
#define TESSERA_MIN_ZOOM 0
#endif
#ifndef TESSERA_MAX_ZOOM
#define TESSERA_MAX_ZOOM 22
#endif

/*
 * A geographic position.
 *
 * double, not float. At zoom 16 one pixel is about 2.4e-6 degrees of
 * longitude, and float carries roughly 7 significant decimal digits -- so at a
 * longitude around 50 degrees the smallest representable step is about 4e-6
 * degrees, which is coarser than a pixel, and worse above zoom 16. Positions
 * are passed around a handful of times per frame rather than per pixel, so the
 * cost of the wider type is not measurable.
 */
typedef struct
{
    double latitude;   /* degrees, north positive */
    double longitude;  /* degrees, east positive  */
} tess_geo;

/* An integer tile address. */
typedef struct
{
    int32_t x;
    int32_t y;
    int32_t zoom;
} tess_tile;

/* A pixel offset inside a single tile, 0..255. */
typedef struct
{
    int32_t x;
    int32_t y;
} tess_pixel;

/* A geographic bounding box. */
typedef struct
{
    tess_geo south_west;
    tess_geo north_east;
} tess_bounds;

/* ------------------------------------------------------------------ basics */

/* Number of tiles per axis at `zoom`, i.e. 2^zoom.
 *
 * Computed by shifting, not with pow(). pow(2, 16) is not guaranteed to return
 * exactly 65536.0 on every libm, and one ulp low truncates to 65535 when it
 * lands in an int -- which puts the entire map one tile out. */
int32_t tess_tiles_per_axis(int zoom);

/* Clamp a latitude into the projectable range. */
double tess_clamp_latitude(double latitude);

/* Wrap a longitude into [-180, 180). */
double tess_wrap_longitude(double longitude);

/* True if `zoom` is inside [TESSERA_MIN_ZOOM, TESSERA_MAX_ZOOM]. */
bool tess_zoom_is_valid(int zoom);

/*
 * The smallest zoom at which the world is at least `width_px` across.
 *
 * Below it the whole world is narrower than the viewport, so the same place
 * appears more than once on screen and "where is this position" stops having
 * one answer. Everything that maps between screen and ground assumes it has
 * one, so the view clamps to this rather than producing a second copy of the
 * world that nothing else knows about.
 *
 * At 256 pixels per tile that is zoom 1 for a 480-pixel viewport and zoom 3
 * for a 1024-pixel one, so on most displays it costs nothing. Never returns
 * less than TESSERA_MIN_ZOOM or more than TESSERA_MAX_ZOOM.
 */
int tess_min_zoom_for_width(int32_t width_px);

/* ------------------------------------------------- geographic <-> tile ---- */

/*
 * Continuous tile coordinate: the integer part is the tile address, the
 * fractional part is the position within it. Everything else is derived from
 * this, so the forward and inverse transforms cannot drift apart.
 */
void tess_geo_to_tile_f(tess_geo position, int zoom, double *out_x, double *out_y);

/* The tile containing `position`. */
tess_tile tess_geo_to_tile(tess_geo position, int zoom);

/* Pixel offset of `position` within its own tile. */
tess_pixel tess_geo_to_pixel_in_tile(tess_geo position, int zoom);

/*
 * Inverse of tess_geo_to_tile_f: the position at continuous tile coordinate
 * (x, y).
 *
 * Fractional coordinates are meaningful -- (x + 0.5, y + 0.5) is the centre of
 * tile (x, y) -- which is what lets a caller turn a screen pixel back into a
 * position. Latitude comes back inside the projectable range by construction.
 */
tess_geo tess_tile_f_to_geo(double x, double y, int zoom);

/* North-west corner of `tile`. */
tess_geo tess_tile_north_west(tess_tile tile);

/* The geographic box `tile` covers. */
tess_bounds tess_tile_bounds(tess_tile tile);

/* --------------------------------------------------------------- distance - */

/*
 * Signed pixel offset from `from` to `to` at `zoom`, in screen axes: +x east,
 * +y south.
 *
 * This is what the widget needs to place one point relative to another, and
 * it is computed in continuous tile space rather than by combining a tile
 * delta with two per-tile pixel offsets. The latter needs a correction term
 * per axis, and those corrections are where sign errors hide.
 */
void tess_pixel_delta(tess_geo from, tess_geo to, int zoom, int32_t *out_dx, int32_t *out_dy);

/* Ground resolution in metres per pixel at `latitude` and `zoom`. Useful for a
 * scale bar, and for deciding whether a zoom change is worth the tile reads. */
double tess_metres_per_pixel(double latitude, int zoom);

/* ------------------------------------------------------------------ bounds - */

/* Smallest box containing all `count` positions. Returns false if count < 1. */
bool tess_bounds_of(const tess_geo *positions, int count, tess_bounds *out);

/* Centre of a bounding box. */
tess_geo tess_bounds_centre(tess_bounds bounds);

/*
 * Largest zoom at which `bounds` still fits inside a `width` x `height` pixel
 * viewport, clamped to [TESSERA_MIN_ZOOM, TESSERA_MAX_ZOOM].
 *
 * Returns TESSERA_MIN_ZOOM when the box does not fit even at the widest zoom,
 * which is the useful answer -- show as much as possible -- rather than a
 * failure the caller has to invent a fallback for.
 */
int tess_zoom_to_fit(tess_bounds bounds, int width_px, int height_px);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PROJECTION_H */
