/* SPDX-License-Identifier: MIT */

#include "tessera/view.h"

#include <math.h>

/*
 * Clamp a zoom into the range this view can actually represent.
 *
 * The lower bound is not simply TESSERA_MIN_ZOOM: it is whichever is larger of
 * that and the zoom at which the world stops being narrower than the widget.
 * Below that point the same position appears at several screen coordinates,
 * and screen_to_geo has no single answer to give.
 */
static int clamp_zoom(const tess_view *view, int zoom)
{
    const int floor_zoom = tess_min_zoom_for_width(view->width);

    if (zoom < floor_zoom) { return floor_zoom; }
    if (zoom > TESSERA_MAX_ZOOM) { return TESSERA_MAX_ZOOM; }
    return zoom;
}

static tess_geo normalise(tess_geo position)
{
    tess_geo out;
    out.latitude = tess_clamp_latitude(position.latitude);
    out.longitude = tess_wrap_longitude(position.longitude);
    return out;
}

/* The view centre as a continuous tile coordinate. Every screen position in
 * this file is an offset from this one value. */
static void centre_tile_f(const tess_view *view, double *out_x, double *out_y)
{
    tess_geo_to_tile_f(view->centre, view->zoom, out_x, out_y);
}

tess_status tess_view_init(tess_view *view, tess_geo centre, int zoom, int32_t width, int32_t height)
{
    if (view == NULL || width <= 0 || height <= 0)
    {
        return TESS_ERR_ARG;
    }

    view->centre = normalise(centre);
    view->width = width;
    view->height = height;
    view->zoom = clamp_zoom(view, zoom);
    return TESS_OK;
}

void tess_view_set_centre(tess_view *view, tess_geo centre)
{
    if (view == NULL)
    {
        return;
    }
    view->centre = normalise(centre);
}

tess_status tess_view_set_size(tess_view *view, int32_t width, int32_t height)
{
    if (view == NULL || width <= 0 || height <= 0)
    {
        return TESS_ERR_ARG;
    }
    view->width = width;
    view->height = height;

    /* A wider widget may make the current zoom unrepresentable. */
    view->zoom = clamp_zoom(view, view->zoom);
    return TESS_OK;
}

tess_rect tess_view_rect(const tess_view *view)
{
    tess_rect rect = {0, 0, 0, 0};
    if (view != NULL)
    {
        rect.x1 = view->width;
        rect.y1 = view->height;
    }
    return rect;
}

tess_tile tess_view_centre_tile(const tess_view *view)
{
    tess_tile tile = {0, 0, 0};
    if (view == NULL)
    {
        return tile;
    }
    return tess_geo_to_tile(view->centre, view->zoom);
}

tess_point tess_view_tile_origin(const tess_view *view, tess_tile tile)
{
    tess_point origin = {0, 0};
    if (view == NULL)
    {
        return origin;
    }

    double cx = 0.0, cy = 0.0;
    centre_tile_f(view, &cx, &cy);

    /* The tile's north-west corner is (tile.x, tile.y) in tile units; the view
     * centre is (cx, cy) and sits at (width/2, height/2) on screen.
     *
     * x takes the short way round, for the same reason tess_pixel_delta does:
     * a grid column just west of the centre has a wrapped address when the
     * centre is near the antimeridian, and without this it would be drawn a
     * whole world away to the east instead of just off the left edge. */
    const double n = (double) tess_tiles_per_axis(view->zoom);
    double dx = (double) tile.x - cx;

    if (dx > n / 2.0)
    {
        dx -= n;
    }
    else if (dx < -n / 2.0)
    {
        dx += n;
    }

    origin.x = (int32_t) lround((double) view->width / 2.0 + dx * TESSERA_TILE_SIZE);
    origin.y = (int32_t) lround((double) view->height / 2.0
                                + ((double) tile.y - cy) * TESSERA_TILE_SIZE);
    return origin;
}

tess_point tess_view_geo_to_screen(const tess_view *view, tess_geo position)
{
    tess_point point = {0, 0};
    if (view == NULL)
    {
        return point;
    }

    int32_t dx = 0, dy = 0;
    tess_pixel_delta(view->centre, normalise(position), view->zoom, &dx, &dy);

    point.x = view->width / 2 + dx;
    point.y = view->height / 2 + dy;
    return point;
}

tess_geo tess_view_screen_to_geo(const tess_view *view, tess_point point)
{
    tess_geo position = {0.0, 0.0};
    if (view == NULL)
    {
        return position;
    }

    double cx = 0.0, cy = 0.0;
    centre_tile_f(view, &cx, &cy);

    const double x = cx + (double)(point.x - view->width / 2) / (double) TESSERA_TILE_SIZE;
    const double y = cy + (double)(point.y - view->height / 2) / (double) TESSERA_TILE_SIZE;

    return normalise(tess_tile_f_to_geo(x, y, view->zoom));
}

void tess_view_pan(tess_view *view, int32_t dx, int32_t dy)
{
    if (view == NULL || (dx == 0 && dy == 0))
    {
        return;
    }

    double cx = 0.0, cy = 0.0;
    centre_tile_f(view, &cx, &cy);

    cx += (double) dx / (double) TESSERA_TILE_SIZE;
    cy += (double) dy / (double) TESSERA_TILE_SIZE;

    /* Clamping y rather than wrapping it: there is nothing north of the north
     * edge to pan onto, and letting the centre leave the world would make
     * every tile in the grid invalid at once -- a blank screen the user has no
     * obvious way back from. x is wrapped by normalise(), so the antimeridian
     * is crossed rather than hit. */
    const double n = (double) tess_tiles_per_axis(view->zoom);
    if (cy < 0.0) { cy = 0.0; }
    if (cy > n)   { cy = n; }

    view->centre = normalise(tess_tile_f_to_geo(cx, cy, view->zoom));
}

void tess_view_zoom_at(tess_view *view, int delta, tess_point anchor)
{
    if (view == NULL || delta == 0)
    {
        return;
    }

    const int target = clamp_zoom(view, view->zoom + delta);
    if (target == view->zoom)
    {
        return;  /* already at the limit: do not shift the centre for nothing */
    }

    /* Hold the position under the anchor fixed: work out what it is at the old
     * zoom, then move the centre so that it lands back under the same pixel at
     * the new one. */
    const tess_geo held = tess_view_screen_to_geo(view, anchor);

    view->zoom = target;

    double hx = 0.0, hy = 0.0;
    tess_geo_to_tile_f(held, target, &hx, &hy);

    const double cx = hx - (double)(anchor.x - view->width / 2) / (double) TESSERA_TILE_SIZE;
    const double cy = hy - (double)(anchor.y - view->height / 2) / (double) TESSERA_TILE_SIZE;

    view->centre = normalise(tess_tile_f_to_geo(cx, cy, target));
}

tess_status tess_view_fit(tess_view *view, const tess_geo *positions, int count, int32_t margin)
{
    if (view == NULL || positions == NULL || count < 1)
    {
        return TESS_ERR_ARG;
    }

    tess_bounds bounds;
    if (!tess_bounds_of(positions, count, &bounds))
    {
        return TESS_ERR_ARG;
    }

    view->centre = normalise(tess_bounds_centre(bounds));

    /* One position has no extent, so there is no zoom it implies. Centring and
     * leaving the zoom alone is what a caller centring on one thing wants;
     * jumping to the maximum would be a surprise. */
    if (count > 1)
    {
        int32_t usable_w = view->width - 2 * margin;
        int32_t usable_h = view->height - 2 * margin;
        if (usable_w < 1) { usable_w = 1; }
        if (usable_h < 1) { usable_h = 1; }

        view->zoom = clamp_zoom(view, tess_zoom_to_fit(bounds, usable_w, usable_h));
    }
    return TESS_OK;
}
