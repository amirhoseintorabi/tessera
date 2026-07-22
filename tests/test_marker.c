/* SPDX-License-Identifier: MIT */
/*
 * Tests for marker placement and the off-screen edge arrow.
 *
 * The off-screen case is the interesting one. Splitting the viewport into four
 * triangular regions and writing a formula per region is the approach that
 * suggests itself, and it needs four formulas that agree at three boundaries
 * each, plus the two degenerate cases -- a marker exactly on a diagonal, and
 * one exactly at the centre.
 *
 * So the tests below sweep the full circle rather than sampling a few
 * directions: any region-based implementation is wrong somewhere, and the
 * place it is wrong is usually a boundary between regions.
 */

#include "tessera/marker.h"

#include "check.h"

/* The Royal Observatory, Greenwich -- a fixed, public reference point. On the
 * prime meridian, so a longitude sign error shows up at once. */
static const tess_geo kSite = {51.47788, -0.00159};

static tess_view make_view(int zoom)
{
    tess_view view;
    CHECK_STATUS(tess_view_init(&view, kSite, zoom, 480, 272), TESS_OK);
    return view;
}

static void test_normalise_degrees(void)
{
    begin("degrees normalise into [0, 360)");

    CHECK_EQ_I(tess_normalise_degrees(0), 0);
    CHECK_EQ_I(tess_normalise_degrees(359), 359);
    CHECK_EQ_I(tess_normalise_degrees(360), 0);
    CHECK_EQ_I(tess_normalise_degrees(361), 1);
    CHECK_EQ_I(tess_normalise_degrees(-1), 359);
    CHECK_EQ_I(tess_normalise_degrees(-360), 0);
    CHECK_EQ_I(tess_normalise_degrees(-361), 359);
    CHECK_EQ_I(tess_normalise_degrees(3600), 0);
    CHECK_EQ_I(tess_normalise_degrees(-3601), 359);
}

static void test_bearing_convention(void)
{
    begin("bearing is compass: 0 is up the screen, increasing clockwise");

    /* Screen y grows downwards while north is up, which is the sign everyone
     * gets wrong once. Naming the four cardinal directions explicitly is the
     * cheapest possible guard against it. */
    CHECK_EQ_I(tess_bearing_from_offset(0, -10), 0);    /* north */
    CHECK_EQ_I(tess_bearing_from_offset(10, 0), 90);    /* east  */
    CHECK_EQ_I(tess_bearing_from_offset(0, 10), 180);   /* south */
    CHECK_EQ_I(tess_bearing_from_offset(-10, 0), 270);  /* west  */

    CHECK_EQ_I(tess_bearing_from_offset(10, -10), 45);
    CHECK_EQ_I(tess_bearing_from_offset(10, 10), 135);
    CHECK_EQ_I(tess_bearing_from_offset(-10, 10), 225);
    CHECK_EQ_I(tess_bearing_from_offset(-10, -10), 315);

    /* No direction at all: 0, not a NaN and not whatever atan2(0,0) gives. */
    CHECK_EQ_I(tess_bearing_from_offset(0, 0), 0);
}

static void test_marker_set(void)
{
    begin("setting a marker clamps its position and terminates its label");

    tess_marker marker;
    const tess_geo pole = {95.0, 400.0};
    tess_marker_set(&marker, pole, "Waypoint");

    CHECK_NEAR(marker.position.latitude, TESSERA_MAX_LATITUDE, 1e-9);
    CHECK(marker.position.longitude >= -180.0 && marker.position.longitude < 180.0);
    CHECK(marker.visible);
    CHECK(!marker.has_heading);
    CHECK_STR_EQ(marker.label, "Waypoint");

    /* A label longer than the field is truncated with the terminator intact.
     * Labels come from places the widget does not control, so the length is
     * not something to take on trust. */
    const char *long_label = "0123456789012345678901234567890123456789";
    tess_marker_set(&marker, kSite, long_label);
    CHECK_EQ_I(strlen(marker.label), TESS_MARKER_LABEL_MAX - 1);
    CHECK_EQ_I(marker.label[TESS_MARKER_LABEL_MAX - 1], 0);

    tess_marker_set(&marker, kSite, NULL);
    CHECK_STR_EQ(marker.label, "");

    tess_marker_set(NULL, kSite, "x");  /* must not crash */
}

static void test_marker_heading(void)
{
    begin("a heading is normalised and marks the marker as having one");

    tess_marker marker;
    tess_marker_set(&marker, kSite, "Focus");
    CHECK(!marker.has_heading);

    tess_marker_set_heading(&marker, 450);
    CHECK(marker.has_heading);
    CHECK_EQ_I(marker.heading_deg, 90);

    tess_marker_set_heading(&marker, -90);
    CHECK_EQ_I(marker.heading_deg, 270);

    /* Setting the position again clears the heading: a position with a stale
     * orientation attached is worse than one with none. */
    tess_marker_set(&marker, kSite, "Focus");
    CHECK(!marker.has_heading);

    tess_marker_set_heading(NULL, 0);
}

static void test_marker_on_screen(void)
{
    begin("a marker inside the viewport is placed where it is");

    const tess_view view = make_view(14);

    const tess_marker_placement centre = tess_marker_locate(&view, view.centre, 16);
    CHECK(centre.on_screen);
    CHECK_EQ_I(centre.point.x, 240);
    CHECK_EQ_I(centre.point.y, 136);
    CHECK_EQ_I(centre.bearing_deg, 0);

    /* A point a little east is still on screen, and its bearing is east. */
    const tess_geo east = tess_view_screen_to_geo(&view, (tess_point){300, 136});
    const tess_marker_placement p = tess_marker_locate(&view, east, 16);
    CHECK(p.on_screen);
    CHECK(p.point.x >= 299 && p.point.x <= 301);
    CHECK_EQ_I(p.bearing_deg, 90);
}

static void test_inset_boundary_is_consistent(void)
{
    begin("nothing falls between 'too close to the edge' and 'off-screen'");

    const tess_view view = make_view(14);
    const int32_t inset = 16;
    const tess_rect band = tess_rect_inset(tess_view_rect(&view), inset, inset);

    /* Every pixel of the viewport is either inside the inset rectangle -- and
     * so drawn in place -- or outside it and drawn as an arrow. Testing
     * containment against the full rectangle while drawing arrows on the inset
     * one leaves a band where a marker is drawn in place with half of it
     * clipped off the edge. */
    for (int32_t y = 0; y < view.height; y += 4)
    {
        for (int32_t x = 0; x < view.width; x += 4)
        {
            const tess_geo g = tess_view_screen_to_geo(&view, (tess_point){x, y});
            const tess_marker_placement p = tess_marker_locate(&view, g, inset);

            const tess_point at = {x, y};
            const bool expected_inside = tess_rect_contains(band, at);

            /* One pixel of slack for the screen -> geo -> screen round trip. */
            const bool near_edge = (x <= band.x0 + 1) || (x >= band.x1 - 2)
                                || (y <= band.y0 + 1) || (y >= band.y1 - 2);
            if (!near_edge)
            {
                CHECK_EQ_I(p.on_screen, expected_inside);
            }
        }
    }
}

static void test_off_screen_arrow_lands_on_the_edge(void)
{
    begin("an off-screen marker puts its arrow on the inset rectangle");

    const tess_view view = make_view(14);
    const int32_t inset = 16;
    const tess_rect band = tess_rect_inset(tess_view_rect(&view), inset, inset);

    /* Sweep the whole circle in one-degree steps at a radius well outside the
     * viewport. Every one of the 360 must produce an arrow that sits on the
     * boundary of the inset rectangle -- not inside it, not beyond it. A
     * region-based implementation fails this at its region boundaries. */
    for (int degrees = 0; degrees < 360; degrees++)
    {
        const double radians = (double) degrees * 3.14159265358979323846 / 180.0;
        const tess_point far_away = {
            240 + (int32_t) (900.0 * sin(radians)),
            136 - (int32_t) (900.0 * cos(radians)),
        };

        const tess_geo g = tess_view_screen_to_geo(&view, far_away);
        const tess_marker_placement p = tess_marker_locate(&view, g, inset);

        CHECK(!p.on_screen);

        /* On the boundary: within the rectangle, and touching one of its
         * edges. */
        CHECK(p.point.x >= band.x0 && p.point.x <= band.x1);
        CHECK(p.point.y >= band.y0 && p.point.y <= band.y1);

        const bool on_an_edge = (p.point.x == band.x0) || (p.point.x == band.x1)
                             || (p.point.y == band.y0) || (p.point.y == band.y1);
        CHECK(on_an_edge);
    }
}

static void test_off_screen_arrow_points_at_the_marker(void)
{
    begin("the arrow's bearing is the direction of the marker it stands for");

    const tess_view view = make_view(14);

    for (int degrees = 0; degrees < 360; degrees += 5)
    {
        const double radians = (double) degrees * 3.14159265358979323846 / 180.0;
        const tess_point far_away = {
            240 + (int32_t) (900.0 * sin(radians)),
            136 - (int32_t) (900.0 * cos(radians)),
        };

        const tess_geo g = tess_view_screen_to_geo(&view, far_away);
        const tess_marker_placement p = tess_marker_locate(&view, g, 16);

        /* Within two degrees: the target position is rounded to whole pixels
         * at a radius of 900, which is about 0.06 degrees per pixel, and the
         * screen round trip costs a fraction more. */
        int error = (int) p.bearing_deg - degrees;
        if (error > 180)  { error -= 360; }
        if (error < -180) { error += 360; }
        CHECK(error >= -2 && error <= 2);
    }
}

static void test_arrow_direction_and_position_agree(void)
{
    begin("the arrow sits on the edge it points at");

    const tess_view view = make_view(14);
    const tess_rect band = tess_rect_inset(tess_view_rect(&view), 16, 16);

    struct
    {
        const char *name;
        tess_point at;
        int32_t expect_bearing;
    } cases[] = {
        {"due north", {240, -800}, 0},
        {"due east",  {1200, 136}, 90},
        {"due south", {240, 900},  180},
        {"due west",  {-800, 136}, 270},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        const tess_geo g = tess_view_screen_to_geo(&view, cases[i].at);
        const tess_marker_placement p = tess_marker_locate(&view, g, 16);

        CHECK(!p.on_screen);
        CHECK_EQ_I(p.bearing_deg, cases[i].expect_bearing);

        switch (cases[i].expect_bearing)
        {
        case 0:   CHECK_EQ_I(p.point.y, band.y0); break;
        case 90:  CHECK_EQ_I(p.point.x, band.x1); break;
        case 180: CHECK_EQ_I(p.point.y, band.y1); break;
        default:  CHECK_EQ_I(p.point.x, band.x0); break;
        }
    }
}

static void test_corner_case(void)
{
    begin("a marker exactly on the diagonal lands on a corner");

    /* The viewport is 480x272 with a 16-pixel inset, so the inset rectangle is
     * 448x240 and its diagonal is not at 45 degrees. That asymmetry is what a
     * region-based implementation gets wrong: the region boundary comes from
     * the rectangle, but the formula inside the region quietly assumes the
     * other axis. */
    const tess_view view = make_view(14);
    const tess_rect band = tess_rect_inset(tess_view_rect(&view), 16, 16);

    const double hx = (double) tess_rect_width(band) / 2.0;
    const double hy = (double) tess_rect_height(band) / 2.0;

    /* Straight out along the corner direction, ten times as far. */
    const tess_point far_corner = {240 + (int32_t)(hx * 10.0), 136 + (int32_t)(hy * 10.0)};
    const tess_geo g = tess_view_screen_to_geo(&view, far_corner);
    const tess_marker_placement p = tess_marker_locate(&view, g, 16);

    CHECK(!p.on_screen);
    CHECK(p.point.x >= band.x1 - 2 && p.point.x <= band.x1);
    CHECK(p.point.y >= band.y1 - 2 && p.point.y <= band.y1);
}

static void test_degenerate_inset(void)
{
    begin("an inset larger than the widget leaves the arrow at the centre");

    const tess_view view = make_view(14);

    /* Not a hypothetical: a 31-pixel marker bitmap on a narrow status strip
     * would do it. The answer has to be representable rather than an inverted
     * rectangle that makes every subsequent containment test lie. */
    const tess_geo g = tess_view_screen_to_geo(&view, (tess_point){2000, 2000});
    const tess_marker_placement p = tess_marker_locate(&view, g, 400);

    CHECK(!p.on_screen);
    CHECK(p.point.x >= 0 && p.point.x <= view.width);
    CHECK(p.point.y >= 0 && p.point.y <= view.height);
}

static void test_null_safety(void)
{
    begin("a NULL view is refused, not dereferenced");

    const tess_marker_placement p = tess_marker_locate(NULL, kSite, 16);
    CHECK(p.on_screen);
    CHECK_EQ_I(p.point.x, 0);
}

int main(void)
{
    printf("tessera: markers and edge arrows\n");

    test_normalise_degrees();
    test_bearing_convention();
    test_marker_set();
    test_marker_heading();
    test_marker_on_screen();
    test_inset_boundary_is_consistent();
    test_off_screen_arrow_lands_on_the_edge();
    test_off_screen_arrow_points_at_the_marker();
    test_arrow_direction_and_position_agree();
    test_corner_case();
    test_degenerate_inset();
    test_null_safety();

    REPORT("marker");
}
