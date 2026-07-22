/* SPDX-License-Identifier: MIT */
/*
 * Tests for the viewport.
 *
 * The property that matters most is that the three ways of asking "where does
 * this go on screen" agree with each other: the tile blit origin, the
 * geographic-to-screen transform, and its inverse. Derived separately they
 * need per-axis correction terms to reconcile, and the corrections are where
 * sign errors hide -- so most of what is below is a cross-check between
 * transforms rather than a fixed expected value.
 */

#include "tessera/view.h"

#include "check.h"
#include "fixture.h"


/* The lowest zoom a 480-pixel-wide view can represent. Below it the world is
 * narrower than the widget, the same place appears twice on screen, and
 * screen_to_geo has no single answer -- so the view clamps to it. */
#define LOW_ZOOM  tess_min_zoom_for_width(480)

static tess_view make_view(int zoom)
{
    tess_view view;
    CHECK_STATUS(tess_view_init(&view, test_site(), zoom, 480, 272), TESS_OK);
    return view;
}

static void test_init(void)
{
    begin("a view is always in a drawable state");

    tess_view view;
    CHECK_STATUS(tess_view_init(&view, test_site(), 14, 480, 272), TESS_OK);
    CHECK_EQ_I(view.zoom, 14);
    CHECK_EQ_I(view.width, 480);
    CHECK_EQ_I(view.height, 272);

    /* Zoom outside the range the map ships tiles for is clamped, not rejected:
     * a caller restoring a saved zoom from a unit that shipped a different tile
     * set should get the nearest usable view, not a failure. */
    CHECK_STATUS(tess_view_init(&view, test_site(), 99, 480, 272), TESS_OK);
    CHECK_EQ_I(view.zoom, TESSERA_MAX_ZOOM);
    CHECK_STATUS(tess_view_init(&view, test_site(), 0, 480, 272), TESS_OK);
    CHECK_EQ_I(view.zoom, LOW_ZOOM);
    CHECK(LOW_ZOOM >= TESSERA_MIN_ZOOM);

    /* A wider widget raises that floor, and resizing re-applies it. */
    tess_view wide;
    CHECK_STATUS(tess_view_init(&wide, test_site(), 0, 4000, 272), TESS_OK);
    CHECK_EQ_I(wide.zoom, tess_min_zoom_for_width(4000));
    CHECK(wide.zoom > LOW_ZOOM);

    /* Latitude beyond the projectable range is clamped. Without it the
     * projection runs away near the poles, and a GNSS receiver reporting a
     * garbage latitude during cold start is the ordinary way that happens. */
    const tess_geo pole = {90.0, 0.0};
    CHECK_STATUS(tess_view_init(&view, pole, 12, 480, 272), TESS_OK);
    CHECK_NEAR(view.centre.latitude, TESSERA_MAX_LATITUDE, 1e-9);

    const tess_geo wrapped = {0.0, 540.0};
    CHECK_STATUS(tess_view_init(&view, wrapped, 12, 480, 272), TESS_OK);
    CHECK_NEAR(view.centre.longitude, 180.0 - 360.0, 1e-9);

    CHECK_STATUS(tess_view_init(NULL, test_site(), 12, 480, 272), TESS_ERR_ARG);
    CHECK_STATUS(tess_view_init(&view, test_site(), 12, 0, 272), TESS_ERR_ARG);
    CHECK_STATUS(tess_view_init(&view, test_site(), 12, 480, -1), TESS_ERR_ARG);
}

static void test_centre_is_at_the_middle_of_the_screen(void)
{
    begin("the view centre lands in the middle of the widget");

    for (int zoom = LOW_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const tess_view view = make_view(zoom);
        const tess_point p = tess_view_geo_to_screen(&view, view.centre);
        CHECK_EQ_I(p.x, 240);
        CHECK_EQ_I(p.y, 136);
    }
}

static void test_tile_origin_agrees_with_the_projection(void)
{
    begin("a tile's blit origin puts its own north-west corner at that pixel");

    /* If these two disagree the map is drawn correctly and the markers are
     * placed on the wrong part of it, or the reverse -- which is the visible
     * form of having two independent formulas. */
    for (int zoom = LOW_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const tess_view view = make_view(zoom);
        const tess_tile centre = tess_view_centre_tile(&view);

        for (int32_t dy = -2; dy <= 2; dy++)
        {
            for (int32_t dx = -2; dx <= 2; dx++)
            {
                tess_tile tile = centre;
                tile.x += dx;
                tile.y += dy;

                /* Only for tiles that exist. A row above the north edge has no
                 * north-west corner inside the projectable range, so there is
                 * nothing to compare against -- and nothing is drawn there. */
                if (!tess_tile_is_valid(tile))
                {
                    continue;
                }

                const tess_point origin = tess_view_tile_origin(&view, tile);
                const tess_point corner = tess_view_geo_to_screen(&view, tess_tile_north_west(tile));

                CHECK_EQ_I(origin.x, corner.x);
                CHECK_EQ_I(origin.y, corner.y);
            }
        }
    }
}

static void test_adjacent_tiles_abut(void)
{
    begin("neighbouring tiles are exactly 256 pixels apart, with no seam");

    const tess_view view = make_view(14);
    const tess_tile centre = tess_view_centre_tile(&view);

    tess_tile east = centre;
    east.x += 1;
    tess_tile south = centre;
    south.y += 1;

    const tess_point o = tess_view_tile_origin(&view, centre);
    const tess_point e = tess_view_tile_origin(&view, east);
    const tess_point s = tess_view_tile_origin(&view, south);

    CHECK_EQ_I(e.x - o.x, TESSERA_TILE_SIZE);
    CHECK_EQ_I(e.y, o.y);
    CHECK_EQ_I(s.y - o.y, TESSERA_TILE_SIZE);
    CHECK_EQ_I(s.x, o.x);
}

static void test_screen_to_geo_is_the_inverse(void)
{
    begin("screen_to_geo and geo_to_screen invert each other");

    for (int zoom = LOW_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const tess_view view = make_view(zoom);

        for (int32_t y = 0; y < 272; y += 17)
        {
            for (int32_t x = 0; x < 480; x += 23)
            {
                const tess_geo position = tess_view_screen_to_geo(&view, P(x, y));
                const tess_point back = tess_view_geo_to_screen(&view, position);

                /* One pixel of slack: both directions round to integers, and
                 * the projection's own round-trip lands a few ulp off. */
                CHECK(back.x >= x - 1 && back.x <= x + 1);
                CHECK(back.y >= y - 1 && back.y <= y + 1);
            }
        }
    }
}

static void test_screen_axes_point_the_right_way(void)
{
    begin("east is +x and north is -y");

    const tess_view view = make_view(14);

    const tess_geo east = {test_site().latitude, test_site().longitude + 0.01};
    const tess_geo north = {test_site().latitude + 0.01, test_site().longitude};

    const tess_point pe = tess_view_geo_to_screen(&view, east);
    const tess_point pn = tess_view_geo_to_screen(&view, north);

    CHECK(pe.x > 240);
    CHECK_EQ_I(pe.y, 136);
    CHECK(pn.y < 136);
    CHECK_EQ_I(pn.x, 240);
}

static void test_pan_moves_by_the_pixels_asked_for(void)
{
    begin("panning by n pixels moves the map by n pixels");

    /* Panning in pixels rather than in degrees. A fixed step in degrees is
     * about 20 pixels at zoom 10 and 1,300 at zoom 16, so the same gesture
     * either does nothing visible or throws the view clear of every loaded
     * tile. */
    for (int zoom = LOW_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        tess_view view = make_view(zoom);
        const tess_geo before = view.centre;

        tess_view_pan(&view, 100, 60);

        /* The position that was under (340, 196) is now under the centre. */
        const tess_point p = tess_view_geo_to_screen(&view, before);
        CHECK(p.x >= 240 - 100 - 1 && p.x <= 240 - 100 + 1);
        CHECK(p.y >= 136 - 60 - 1 && p.y <= 136 - 60 + 1);
    }
}

static void test_pan_round_trip(void)
{
    begin("panning back returns to where it started");

    tess_view view = make_view(15);
    const tess_geo before = view.centre;

    for (int i = 0; i < 40; i++)
    {
        tess_view_pan(&view, 37, -19);
    }
    for (int i = 0; i < 40; i++)
    {
        tess_view_pan(&view, -37, 19);
    }

    /* Nothing accumulates, because the pan moves the one quantity every
     * position is derived from rather than adjusting a set of cached offsets. */
    const tess_point p = tess_view_geo_to_screen(&view, before);
    CHECK(p.x >= 239 && p.x <= 241);
    CHECK(p.y >= 135 && p.y <= 137);
}

static void test_pan_is_bounded_north_and_south(void)
{
    begin("panning off the top of the world stops at the top of the world");

    tess_view view = make_view(12);

    /* Bounded loops, and a condition rather than a fixed count: at zoom 12 the
     * world is 4096 tiles tall, so how many 1000-pixel pans it takes to reach
     * the edge depends on where the view started. */
    int steps = 0;
    while (view.centre.latitude < 84.0 && steps++ < 5000)
    {
        tess_view_pan(&view, 0, -1000);
    }
    CHECK(view.centre.latitude > 84.0);
    CHECK(view.centre.latitude <= TESSERA_MAX_LATITUDE + 1e-9);

    /* And it stays there rather than wrapping round to the south pole. */
    tess_view_pan(&view, 0, -100000);
    CHECK(view.centre.latitude > 84.0);
    CHECK(view.centre.latitude <= TESSERA_MAX_LATITUDE + 1e-9);

    steps = 0;
    while (view.centre.latitude > -84.0 && steps++ < 5000)
    {
        tess_view_pan(&view, 0, 1000);
    }
    CHECK(view.centre.latitude < -84.0);
    CHECK(view.centre.latitude >= -TESSERA_MAX_LATITUDE - 1e-9);

    tess_view_pan(&view, 0, 100000);
    CHECK(view.centre.latitude < -84.0);
    CHECK(view.centre.latitude >= -TESSERA_MAX_LATITUDE - 1e-9);
}

static void test_pan_crosses_the_antimeridian(void)
{
    begin("panning east past 180 degrees comes out at -180");

    tess_view view;
    const tess_geo near_dateline = {0.0, 179.99};
    CHECK_STATUS(tess_view_init(&view, near_dateline, 10, 480, 272), TESS_OK);

    for (int i = 0; i < 20; i++)
    {
        tess_view_pan(&view, 200, 0);
    }

    CHECK(view.centre.longitude >= -180.0);
    CHECK(view.centre.longitude < 180.0);
    CHECK(view.centre.longitude < 0.0);  /* it did cross, rather than clamping */
}

static void test_zoom_holds_the_anchor(void)
{
    begin("zooming keeps the position under the anchor pixel under it");

    for (int zoom = LOW_ZOOM; zoom < TESSERA_MAX_ZOOM; zoom++)
    {
        tess_view view = make_view(zoom);
        const tess_point anchor = P(400, 60);
        const tess_geo held = tess_view_screen_to_geo(&view, anchor);

        tess_view_zoom_at(&view, +1, anchor);
        CHECK_EQ_I(view.zoom, zoom + 1);

        const tess_point p = tess_view_geo_to_screen(&view, held);
        CHECK(p.x >= anchor.x - 1 && p.x <= anchor.x + 1);
        CHECK(p.y >= anchor.y - 1 && p.y <= anchor.y + 1);
    }
}

static void test_zoom_at_the_centre_does_not_move_the_centre(void)
{
    begin("zooming about the middle leaves the centre alone");

    tess_view view = make_view(13);
    const tess_geo before = view.centre;

    tess_view_zoom_at(&view, +1, P(240, 136));
    CHECK_NEAR(view.centre.latitude, before.latitude, 1e-6);
    CHECK_NEAR(view.centre.longitude, before.longitude, 1e-6);

    tess_view_zoom_at(&view, -1, P(240, 136));
    CHECK_EQ_I(view.zoom, 13);
    CHECK_NEAR(view.centre.latitude, before.latitude, 1e-6);
    CHECK_NEAR(view.centre.longitude, before.longitude, 1e-6);
}

static void test_zoom_clamps_without_side_effects(void)
{
    begin("zooming past the limit changes nothing at all");

    tess_view view = make_view(TESSERA_MAX_ZOOM);
    const tess_geo before = view.centre;

    /* An off-centre anchor is the case that matters: a clamp implemented as
     * "recompute anyway, then clamp the zoom" would shift the centre every
     * time the user pressed a button that should have done nothing. */
    tess_view_zoom_at(&view, +1, P(10, 10));
    CHECK_EQ_I(view.zoom, TESSERA_MAX_ZOOM);
    CHECK_NEAR(view.centre.latitude, before.latitude, 1e-12);
    CHECK_NEAR(view.centre.longitude, before.longitude, 1e-12);

    tess_view low = make_view(LOW_ZOOM);
    const tess_geo low_before = low.centre;
    tess_view_zoom_at(&low, -3, P(470, 260));
    CHECK_EQ_I(low.zoom, LOW_ZOOM);
    CHECK_NEAR(low.centre.latitude, low_before.latitude, 1e-12);
}

static void test_fit(void)
{
    begin("fit frames every position with the margin kept clear");

    tess_view view = make_view(16);

    const tess_geo points[] = {
        {51.4700, -0.0400},
        {51.4900,  0.0000},
        {51.4800, -0.0200},
    };

    CHECK_STATUS(tess_view_fit(&view, points, 3, 20), TESS_OK);
    CHECK(view.zoom >= LOW_ZOOM && view.zoom <= TESSERA_MAX_ZOOM);

    for (int i = 0; i < 3; i++)
    {
        const tess_point p = tess_view_geo_to_screen(&view, points[i]);
        CHECK(p.x >= 0 && p.x < view.width);
        CHECK(p.y >= 0 && p.y < view.height);
    }

    /* One point has no extent, so there is no zoom it implies: centre on it
     * and leave the zoom where the user put it. */
    tess_view single = make_view(13);
    CHECK_STATUS(tess_view_fit(&single, points, 1, 20), TESS_OK);
    CHECK_EQ_I(single.zoom, 13);
    CHECK_NEAR(single.centre.latitude, points[0].latitude, 1e-9);

    CHECK_STATUS(tess_view_fit(&view, points, 0, 20), TESS_ERR_ARG);
    CHECK_STATUS(tess_view_fit(&view, NULL, 3, 20), TESS_ERR_ARG);
    CHECK_STATUS(tess_view_fit(NULL, points, 3, 20), TESS_ERR_ARG);

    /* A margin wider than the widget must not produce a zero or negative
     * viewport for the fit arithmetic to divide by. */
    CHECK_STATUS(tess_view_fit(&view, points, 3, 5000), TESS_OK);
    CHECK(view.zoom >= LOW_ZOOM);
}

static void test_resize_keeps_the_centre(void)
{
    begin("resizing the widget does not move the map");

    tess_view view = make_view(14);
    const tess_geo before = view.centre;

    CHECK_STATUS(tess_view_set_size(&view, 800, 480), TESS_OK);
    CHECK_NEAR(view.centre.latitude, before.latitude, 1e-12);

    const tess_point p = tess_view_geo_to_screen(&view, before);
    CHECK_EQ_I(p.x, 400);
    CHECK_EQ_I(p.y, 240);

    CHECK_STATUS(tess_view_set_size(&view, 0, 480), TESS_ERR_ARG);
    CHECK_STATUS(tess_view_set_size(NULL, 800, 480), TESS_ERR_ARG);
}

static void test_null_safety(void)
{
    begin("NULL views are refused, not dereferenced");

    tess_view_set_centre(NULL, test_site());
    tess_view_pan(NULL, 1, 1);
    tess_view_zoom_at(NULL, 1, P(0, 0));

    const tess_rect r = tess_view_rect(NULL);
    CHECK(tess_rect_is_empty(r));

    const tess_tile t = tess_view_centre_tile(NULL);
    CHECK_EQ_I(t.zoom, 0);

    const tess_point p = tess_view_geo_to_screen(NULL, test_site());
    CHECK_EQ_I(p.x, 0);
    CHECK_EQ_I(p.y, 0);
}

static void test_tile_origin_wraps_at_the_antimeridian(void)
{
    begin("tiles west of a view near the dateline are drawn to the west");

    /* Centre just east of 180. The grid columns to its west have addresses
     * that wrapped to the far side of the world, and they still have to be
     * blitted immediately to the left -- not a whole world away to the right,
     * which is where the raw address difference would put them. */
    tess_view view;
    const tess_geo near_dateline = {0.0, -179.99};
    CHECK_STATUS(tess_view_init(&view, near_dateline, 10, 480, 272), TESS_OK);

    const tess_tile centre = tess_view_centre_tile(&view);
    const int32_t n = tess_tiles_per_axis(10);
    CHECK_EQ_I(centre.x, 0);

    tess_tile west = centre;
    west.x = n - 1;  /* the column immediately west, wrapped */

    const tess_point o = tess_view_tile_origin(&view, centre);
    const tess_point w = tess_view_tile_origin(&view, west);

    CHECK_EQ_I(o.x - w.x, TESSERA_TILE_SIZE);
    CHECK(w.x < o.x);

    /* And it still agrees with the projection, which wraps the same way. */
    const tess_point corner = tess_view_geo_to_screen(&view, tess_tile_north_west(west));
    CHECK_EQ_I(w.x, corner.x);
    CHECK_EQ_I(w.y, corner.y);
}

int main(void)
{
    printf("tessera: viewport\n");

    test_init();
    test_centre_is_at_the_middle_of_the_screen();
    test_tile_origin_agrees_with_the_projection();
    test_adjacent_tiles_abut();
    test_tile_origin_wraps_at_the_antimeridian();
    test_screen_to_geo_is_the_inverse();
    test_screen_axes_point_the_right_way();
    test_pan_moves_by_the_pixels_asked_for();
    test_pan_round_trip();
    test_pan_is_bounded_north_and_south();
    test_pan_crosses_the_antimeridian();
    test_zoom_holds_the_anchor();
    test_zoom_at_the_centre_does_not_move_the_centre();
    test_zoom_clamps_without_side_effects();
    test_fit();
    test_resize_keeps_the_centre();
    test_null_safety();

    REPORT("view");
}
