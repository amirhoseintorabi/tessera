/* SPDX-License-Identifier: MIT */

#include "tessera/marker.h"

#include <math.h>
#include <string.h>

int16_t tess_normalise_degrees(int degrees)
{
    int value = degrees % 360;
    if (value < 0)
    {
        value += 360;
    }
    return (int16_t) value;
}

int16_t tess_bearing_from_offset(int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0)
    {
        return 0;
    }

    /* atan2(dx, -dy): x is the "sine" argument and negated y the "cosine" one,
     * which rotates the result into compass convention -- 0 pointing up the
     * screen, increasing clockwise -- in one step rather than by adding 90 and
     * flipping a sign afterwards. */
    const double radians = atan2((double) dx, (double) -dy);
    return tess_normalise_degrees((int) lround(radians * (180.0 / M_PI)));
}

void tess_marker_set(tess_marker *marker, tess_geo position, const char *label)
{
    if (marker == NULL)
    {
        return;
    }

    marker->position.latitude = tess_clamp_latitude(position.latitude);
    marker->position.longitude = tess_wrap_longitude(position.longitude);
    marker->heading_deg = 0;
    marker->has_heading = false;
    marker->visible = true;

    memset(marker->label, 0, sizeof(marker->label));
    if (label != NULL)
    {
        /* strncpy into a buffer already zeroed, one short of the end, so the
         * result is terminated whatever the caller passed. Labels come from
         * places the widget does not control -- a route name, a search result
         * -- so the length is not something to assume. */
        strncpy(marker->label, label, sizeof(marker->label) - 1u);
    }
}

void tess_marker_set_heading(tess_marker *marker, int heading_deg)
{
    if (marker == NULL)
    {
        return;
    }
    marker->heading_deg = tess_normalise_degrees(heading_deg);
    marker->has_heading = true;
}

tess_marker_placement tess_marker_locate(const tess_view *view, tess_geo position, int32_t edge_inset)
{
    tess_marker_placement placement;
    placement.on_screen = true;
    placement.point.x = 0;
    placement.point.y = 0;
    placement.bearing_deg = 0;

    if (view == NULL)
    {
        return placement;
    }

    const tess_point screen = tess_view_geo_to_screen(view, position);
    const tess_rect inset = tess_rect_inset(tess_view_rect(view), edge_inset, edge_inset);

    placement.point = screen;

    if (tess_rect_contains(inset, screen))
    {
        /* Still report the bearing: a caller may want it even for a marker
         * that is drawn in place. */
        placement.bearing_deg = tess_bearing_from_offset(screen.x - view->width / 2,
                                                       screen.y - view->height / 2);
        return placement;
    }

    placement.on_screen = false;

    const double cx = (double)(inset.x0 + inset.x1) / 2.0;
    const double cy = (double)(inset.y0 + inset.y1) / 2.0;
    const double dx = (double) screen.x - cx;
    const double dy = (double) screen.y - cy;

    placement.bearing_deg = tess_bearing_from_offset((int32_t) lround(dx), (int32_t) lround(dy));

    /* Half-extents of the inset rectangle. A rectangle that the inset has
     * collapsed leaves the arrow at the centre, which is the only place left. */
    const double hx = (double) tess_rect_width(inset) / 2.0;
    const double hy = (double) tess_rect_height(inset) / 2.0;

    if (hx <= 0.0 || hy <= 0.0 || (dx == 0.0 && dy == 0.0))
    {
        placement.point.x = (int32_t) lround(cx);
        placement.point.y = (int32_t) lround(cy);
        return placement;
    }

    /*
     * Walk out along the ray (dx, dy) until the first edge is met.
     *
     * The ray leaves through the vertical edges after scaling by hx/|dx| and
     * through the horizontal ones after hy/|dy|; whichever comes first is the
     * edge it actually crosses. Taking the smaller of the two is the entire
     * case analysis -- corners and the diagonals fall out of it with the two
     * scales equal, and an axis-aligned ray is handled by the guard below
     * rather than by a special case.
     */
    /* HUGE_VAL rather than INFINITY: INFINITY is a float, and assigning it to
     * a double is an implicit promotion that -Wdouble-promotion rejects. An
     * axis with no component never wins the minimum below, which is what
     * "the ray never leaves through that pair of edges" means. */
    double scale_x = HUGE_VAL;
    double scale_y = HUGE_VAL;

    if (dx != 0.0) { scale_x = hx / fabs(dx); }
    if (dy != 0.0) { scale_y = hy / fabs(dy); }

    const double scale = (scale_x < scale_y) ? scale_x : scale_y;

    placement.point.x = (int32_t) lround(cx + dx * scale);
    placement.point.y = (int32_t) lround(cy + dy * scale);

    /* Rounding can put the point a pixel outside; clamp so the caller can
     * blit without its own bounds check. */
    if (placement.point.x < inset.x0) { placement.point.x = inset.x0; }
    if (placement.point.x > inset.x1) { placement.point.x = inset.x1; }
    if (placement.point.y < inset.y0) { placement.point.y = inset.y0; }
    if (placement.point.y > inset.y1) { placement.point.y = inset.y1; }

    return placement;
}
