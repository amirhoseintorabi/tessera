/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_VIEW_H
#define TESSERA_VIEW_H

/*
 * The viewport: where the map is looking, how far in, and how big the window
 * on screen is.
 *
 * Everything positional is derived from one quantity -- the continuous tile
 * coordinate of the view centre -- so the tile the widget draws, the pixel that
 * tile is blitted at and the pixel a marker lands on cannot disagree. Deriving
 * them separately and reconciling with correction terms is the alternative, and
 * the corrections are where the sign errors end up.
 *
 * Screen axes are +x east, +y south, origin at the widget's top-left.
 */

#include "tessera/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    tess_geo centre;
    int zoom;
    int32_t width;   /* widget size in pixels */
    int32_t height;
} tess_view;

/*
 * Set up a viewport. The latitude is clamped and the longitude wrapped, and
 * the zoom is clamped into the range the map ships tiles for, so a view is
 * always in a drawable state however it was constructed.
 *
 * TESS_ERR_ARG for a NULL view or a non-positive size.
 */
tess_status tess_view_init(tess_view *view, tess_geo centre, int zoom, int32_t width, int32_t height);

/* Move the centre, clamping and wrapping as tess_view_init does. */
void tess_view_set_centre(tess_view *view, tess_geo centre);

/* Change the widget size. Nothing else moves: the same geographic point stays
 * in the middle. */
tess_status tess_view_set_size(tess_view *view, int32_t width, int32_t height);

/* The viewport rectangle in screen coordinates: (0,0) to (width,height). */
tess_rect tess_view_rect(const tess_view *view);

/* The tile containing the view centre. */
tess_tile tess_view_centre_tile(const tess_view *view);

/*
 * Where the top-left corner of `tile` belongs on screen.
 *
 * Computed from the centre's continuous tile coordinate, so it stays correct
 * for tiles well outside the viewport and does not accumulate error over a
 * long sequence of pans -- there is nothing to accumulate into, since the pan
 * moves the centre and every position is recomputed from it.
 */
tess_point tess_view_tile_origin(const tess_view *view, tess_tile tile);

/* Where a geographic position lands on screen. May be outside the viewport. */
tess_point tess_view_geo_to_screen(const tess_view *view, tess_geo position);

/*
 * The geographic position under a screen pixel: the exact inverse of
 * tess_view_geo_to_screen, to within the projection's own round-trip error.
 *
 * Without it a widget cannot drag-to-pan, cannot place a marker where the user
 * tapped, and cannot zoom about anything but its own centre -- and it is
 * tempting to substitute a fixed step in degrees, which is a wildly different
 * distance at each zoom level.
 */
tess_geo tess_view_screen_to_geo(const tess_view *view, tess_point point);

/* Pan by a screen offset: +dx moves the map content left, i.e. the view east. */
void tess_view_pan(tess_view *view, int32_t dx, int32_t dy);

/*
 * Zoom by `delta` levels, keeping the geographic position currently under
 * `anchor` under it afterwards.
 *
 * That is what makes a double-tap or a pinch feel right: the thing you aimed
 * at stays where you aimed. Pass the viewport centre for a plain zoom.
 * Clamped to the map's zoom range, and a clamped zoom leaves the centre alone.
 */
void tess_view_zoom_at(tess_view *view, int delta, tess_point anchor);

/*
 * Frame `count` positions: centre on them and pick the tightest zoom that
 * still shows them all, with `margin` pixels kept clear on every side.
 *
 * TESS_ERR_ARG if there are no positions. A single position is not a
 * degenerate case -- it centres and keeps the current zoom.
 */
tess_status tess_view_fit(tess_view *view, const tess_geo *positions, int count, int32_t margin);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_VIEW_H */
