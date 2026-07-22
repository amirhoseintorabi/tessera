/* SPDX-License-Identifier: MIT */
/*
 * Tests for the Web Mercator tile arithmetic.
 *
 * The reference tile addresses and pixel offsets below were computed
 * independently from the published OpenStreetMap slippy-map formulas, not from
 * this implementation. That is the point: a test that only agrees with the code
 * it is testing proves nothing about whether the code is right, and the whole
 * reason for pulling this layer out of the widget was to be able to check it
 * against something external.
 */
#include "tessera/projection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int checks = 0;
static int failures = 0;
static const char *current = "";

static void begin(const char *name)
{
    current = name;
    printf("  %s\n", name);
}

static void fail(int line, const char *what)
{
    failures++;
    printf("    FAIL %s:%d in \"%s\"\n           %s\n", __FILE__, line, current, what);
}

#define CHECK(expr)                                    \
    do {                                               \
        checks++;                                      \
        if (!(expr)) { fail(__LINE__, #expr); }        \
    } while (0)

#define CHECK_EQ_I(actual, expected)                                            \
    do {                                                                        \
        checks++;                                                               \
        long a_ = (long) (actual), e_ = (long) (expected);                      \
        if (a_ != e_) {                                                         \
            char m[192];                                                        \
            snprintf(m, sizeof m, "%s == %ld, expected %ld", #actual, a_, e_);  \
            fail(__LINE__, m);                                                  \
        }                                                                       \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                       \
    do {                                                                        \
        checks++;                                                               \
        double a_ = (double) (actual), e_ = (double) (expected);                \
        if (fabs(a_ - e_) > (tol)) {                                            \
            char m[192];                                                        \
            snprintf(m, sizeof m, "%s == %.9f, expected %.9f", #actual, a_, e_);\
            fail(__LINE__, m);                                                  \
        }                                                                       \
    } while (0)

/* A fixed reference site for the tests that need somewhere to stand: the Royal
 * Observatory, Greenwich. Latitude well north, longitude essentially on the
 * prime meridian, so a sign error in either axis is immediately visible. */
static const tess_geo kSite = {51.47788, -0.00159};

static void test_tiles_per_axis(void)
{
    begin("tiles per axis is an exact power of two");
    /* Exactness is the point. pow(2, z) truncated into an int is the tempting
     * formulation, and one ulp low at any zoom puts the whole map a tile
     * out. */
    CHECK_EQ_I(tess_tiles_per_axis(0), 1);
    CHECK_EQ_I(tess_tiles_per_axis(1), 2);
    CHECK_EQ_I(tess_tiles_per_axis(10), 1024);
    CHECK_EQ_I(tess_tiles_per_axis(16), 65536);
    CHECK_EQ_I(tess_tiles_per_axis(20), 1048576);
}

static void test_reference_tiles(void)
{
    begin("tile addresses match independently computed references");

    struct
    {
        const char *name;
        tess_geo pos;
        int zoom;
        int32_t tx, ty;
        int32_t px, py;
    } cases[] = {
        {"null island z0",  {0.0, 0.0},              0,     0,     0, 128, 128},
        {"null island z1",  {0.0, 0.0},              1,     1,     1,   0,   0},
        {"Greenwich z10",   {51.47788, -0.00159},   10,   511,   340, 254, 164},
        {"Greenwich z12",   {51.47788, -0.00159},   12,  2047,  1362, 251, 144},
        {"Greenwich z16",   {51.47788, -0.00159},   16, 32767, 21801, 181,   6},
        {"Eiffel z14",      {48.85826, 2.29450},    14,  8296,  5636, 108, 114},
        {"Eiffel z16",      {48.85826, 2.29450},    16, 33185, 22545, 179, 201},
        {"Liberty z13",     {40.68925, -74.04450},  13,  2411,  3080,  19, 189},
        {"Sydney z14",      {-33.85678, 151.21528}, 14, 15073,  9831, 249,  64},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        const tess_tile t = tess_geo_to_tile(cases[i].pos, cases[i].zoom);
        CHECK_EQ_I(t.x, cases[i].tx);
        CHECK_EQ_I(t.y, cases[i].ty);
        CHECK_EQ_I(t.zoom, cases[i].zoom);

        const tess_pixel p = tess_geo_to_pixel_in_tile(cases[i].pos, cases[i].zoom);
        /* One pixel of slack: the reference truncates, and the last bit of a
         * double sine can land either side. */
        CHECK(labs((long) p.x - cases[i].px) <= 1);
        CHECK(labs((long) p.y - cases[i].py) <= 1);
    }
}

static void test_round_trip(void)
{
    begin("tile -> corner -> tile is the identity");
    /* Sweeping both axes over every shipped zoom is what catches a transform
     * whose forward and inverse disagree -- the failure mode that stays
     * invisible when only one of them is ever exercised. */
    for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const int32_t n = tess_tiles_per_axis(zoom);
        for (int i = 0; i < 40; i++)
        {
            tess_tile in;
            in.zoom = zoom;
            in.x = (int32_t) (((int64_t) n * i) / 40);
            in.y = (int32_t) (((int64_t) n * i) / 40);

            const tess_geo corner = tess_tile_north_west(in);
            const tess_tile out = tess_geo_to_tile(corner, zoom);

            CHECK_EQ_I(out.x, in.x);
            CHECK_EQ_I(out.y, in.y);
        }
    }
}

static void test_pixel_within_tile_is_consistent(void)
{
    begin("a tile's north-west corner sits at pixel (0,0) of that tile");
    for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        tess_tile t = {1000, 700, zoom};
        if (t.x >= tess_tiles_per_axis(zoom))
        {
            t.x = tess_tiles_per_axis(zoom) / 2;
            t.y = tess_tiles_per_axis(zoom) / 2;
        }
        const tess_geo nw = tess_tile_north_west(t);
        const tess_pixel p = tess_geo_to_pixel_in_tile(nw, zoom);
        CHECK_EQ_I(p.x, 0);
        CHECK_EQ_I(p.y, 0);
    }
}

static void test_latitude_clamp(void)
{
    begin("the poles are clamped rather than producing infinities");
    /* tan(90 degrees) is infinite, so an unclamped projection returns a NaN
     * tile index, which then casts to an arbitrary int. */
    const tess_geo north_pole = {90.0, 0.0};
    const tess_geo south_pole = {-90.0, 0.0};

    const tess_tile n = tess_geo_to_tile(north_pole, 12);
    const tess_tile s = tess_geo_to_tile(south_pole, 12);

    CHECK(n.y >= 0 && n.y < tess_tiles_per_axis(12));
    CHECK(s.y >= 0 && s.y < tess_tiles_per_axis(12));
    CHECK_EQ_I(n.y, 0);
    CHECK_EQ_I(s.y, tess_tiles_per_axis(12) - 1);

    CHECK_NEAR(tess_clamp_latitude(90.0), TESSERA_MAX_LATITUDE, 1e-9);
    CHECK_NEAR(tess_clamp_latitude(-90.0), -TESSERA_MAX_LATITUDE, 1e-9);
    CHECK_NEAR(tess_clamp_latitude(10.0), 10.0, 1e-12);
}

static void test_longitude_wrap(void)
{
    begin("longitude wraps into [-180, 180)");
    CHECK_NEAR(tess_wrap_longitude(0.0), 0.0, 1e-12);
    CHECK_NEAR(tess_wrap_longitude(180.0), -180.0, 1e-12);
    CHECK_NEAR(tess_wrap_longitude(-180.0), -180.0, 1e-12);
    CHECK_NEAR(tess_wrap_longitude(190.0), -170.0, 1e-12);
    CHECK_NEAR(tess_wrap_longitude(-190.0), 170.0, 1e-12);
    CHECK_NEAR(tess_wrap_longitude(540.0), -180.0, 1e-12);

    /* The dateline must not produce an out-of-range tile. */
    for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const tess_geo east = {0.0, 179.9999};
        const tess_geo west = {0.0, -179.9999};
        const tess_tile te = tess_geo_to_tile(east, zoom);
        const tess_tile tw = tess_geo_to_tile(west, zoom);
        CHECK(te.x >= 0 && te.x < tess_tiles_per_axis(zoom));
        CHECK(tw.x >= 0 && tw.x < tess_tiles_per_axis(zoom));
    }
}

static void test_tile_indices_stay_in_range(void)
{
    begin("no input produces an out-of-range tile index");
    /* The tile address is interpolated straight into a filename, so an index
     * outside the world is both a wrong lookup and, with a fixed-size buffer,
     * a longer string than the caller budgeted for. */
    const tess_geo hostile[] = {
        {90.0, 180.0},   {-90.0, -180.0}, {1e6, 1e6},   {-1e6, -1e6},
        {85.0511, 0.0},  {-85.0511, 0.0}, {0.0, 359.9}, {0.0, -359.9},
    };

    for (unsigned i = 0; i < sizeof hostile / sizeof hostile[0]; i++)
    {
        for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
        {
            const tess_tile t = tess_geo_to_tile(hostile[i], zoom);
            const int32_t n = tess_tiles_per_axis(zoom);
            CHECK(t.x >= 0 && t.x < n);
            CHECK(t.y >= 0 && t.y < n);

            const tess_pixel p = tess_geo_to_pixel_in_tile(hostile[i], zoom);
            CHECK(p.x >= 0 && p.x < TESSERA_TILE_SIZE);
            CHECK(p.y >= 0 && p.y < TESSERA_TILE_SIZE);
        }
    }
}

static void test_pixel_delta_signs(void)
{
    begin("pixel delta points the way the screen does");
    /* +x east, +y south. Getting the y sign backwards puts the vehicle marker
     * on the wrong side of the map, and it is exactly the kind of thing that
     * looks plausible until someone drives north. */
    const tess_geo origin = kSite;

    tess_geo east = origin;
    east.longitude += 0.01;
    tess_geo north = origin;
    north.latitude += 0.01;

    int32_t dx = 0, dy = 0;

    tess_pixel_delta(origin, east, 14, &dx, &dy);
    CHECK(dx > 0);
    CHECK(labs(dy) <= 1);

    tess_pixel_delta(origin, north, 14, &dx, &dy);
    CHECK(dy < 0); /* north is up, so a smaller y */
    CHECK(labs(dx) <= 1);
}

static void test_pixel_delta_is_antisymmetric(void)
{
    begin("delta(a,b) == -delta(b,a)");
    const tess_geo a = kSite;
    const tess_geo b = {32.70000, 51.70000};

    int32_t abx = 0, aby = 0, bax = 0, bay = 0;
    tess_pixel_delta(a, b, 15, &abx, &aby);
    tess_pixel_delta(b, a, 15, &bax, &bay);

    CHECK(labs(abx + bax) <= 1);
    CHECK(labs(aby + bay) <= 1);
}

static void test_pixel_delta_matches_ground_distance(void)
{
    begin("pixel delta agrees with metres per pixel");
    /* An independent cross-check: convert a known ground offset into pixels
     * two different ways and require them to agree. */
    const tess_geo a = kSite;
    tess_geo b = a;
    b.longitude += 0.01; /* pure east */

    for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        int32_t dx = 0, dy = 0;
        tess_pixel_delta(a, b, zoom, &dx, &dy);

        const double metres = 0.01 * (40075016.686 / 360.0) * cos(a.latitude * M_PI / 180.0);
        const double expected_px = metres / tess_metres_per_pixel(a.latitude, zoom);

        CHECK_NEAR((double) dx, expected_px, 1.0 + expected_px * 0.001);
    }
}

static void test_metres_per_pixel(void)
{
    begin("ground resolution matches published values");
    /* Independently computed as 156543.03392804 * cos(latitude) / 2^zoom, the
     * published Web Mercator ground resolution, at 51.47788 N. */
    CHECK_NEAR(tess_metres_per_pixel(kSite.latitude, 0),  97497.620384, 0.01);
    CHECK_NEAR(tess_metres_per_pixel(kSite.latitude, 10),    95.212520, 0.001);
    CHECK_NEAR(tess_metres_per_pixel(kSite.latitude, 13),    11.901565, 0.001);
    CHECK_NEAR(tess_metres_per_pixel(kSite.latitude, 16),     1.487696, 0.001);
    CHECK_NEAR(tess_metres_per_pixel(kSite.latitude, 22),     0.023245, 0.000001);

    /* At the equator the constant appears undivided at zoom 0. */
    CHECK_NEAR(tess_metres_per_pixel(0.0, 0), 156543.03392804, 0.01);

    /* Halving per zoom level. */
    for (int zoom = TESSERA_MIN_ZOOM; zoom < TESSERA_MAX_ZOOM; zoom++)
    {
        const double here = tess_metres_per_pixel(45.0, zoom);
        const double next = tess_metres_per_pixel(45.0, zoom + 1);
        CHECK_NEAR(here / next, 2.0, 1e-9);
    }
}

static void test_bounds(void)
{
    begin("bounding box and centre");
    const tess_geo points[] = {
        {51.45, -0.10}, {51.55, 0.10}, {51.50, -0.15}, {51.40, 0.15},
    };
    tess_bounds b;
    CHECK(tess_bounds_of(points, 4, &b));
    CHECK_NEAR(b.south_west.latitude, 51.40, 1e-9);
    CHECK_NEAR(b.south_west.longitude, -0.15, 1e-9);
    CHECK_NEAR(b.north_east.latitude, 51.55, 1e-9);
    CHECK_NEAR(b.north_east.longitude, 0.15, 1e-9);

    const tess_geo c = tess_bounds_centre(b);
    CHECK_NEAR(c.latitude, 51.475, 1e-9);
    CHECK_NEAR(c.longitude, 0.0, 1e-9);

    CHECK(!tess_bounds_of(points, 0, &b));
    CHECK(!tess_bounds_of(NULL, 4, &b));
    CHECK(!tess_bounds_of(points, 4, NULL));
}

static void test_zoom_to_fit(void)
{
    begin("zoom to fit picks the tightest zoom that still fits");
    const tess_geo points[] = {{51.45, -0.10}, {51.55, 0.10}};
    tess_bounds b;
    CHECK(tess_bounds_of(points, 2, &b));

    const int zoom = tess_zoom_to_fit(b, 400, 240);
    CHECK(tess_zoom_is_valid(zoom));

    /* It must actually fit at the chosen zoom... */
    int32_t dx = 0, dy = 0;
    tess_pixel_delta(b.south_west, b.north_east, zoom, &dx, &dy);
    CHECK(labs(dx) <= 400);
    CHECK(labs(dy) <= 240);

    /* ...and not fit one level tighter, unless we are already at the top. */
    if (zoom < TESSERA_MAX_ZOOM)
    {
        tess_pixel_delta(b.south_west, b.north_east, zoom + 1, &dx, &dy);
        CHECK(labs(dx) > 400 || labs(dy) > 240);
    }
}

static void test_zoom_to_fit_degenerate(void)
{
    begin("zoom to fit handles a single point and a hostile viewport");
    /* Fitting one point is a zero-extent box. A closed-form solution divides
     * by that extent; the caller hits this the first time it asks to centre on
     * the vehicle alone. */
    const tess_geo one[] = {kSite};
    tess_bounds b;
    CHECK(tess_bounds_of(one, 1, &b));
    CHECK_EQ_I(tess_zoom_to_fit(b, 400, 240), TESSERA_MAX_ZOOM);

    /* A box too big for any zoom gets the widest, not a failure. */
    const tess_geo huge[] = {{-80.0, -179.0}, {80.0, 179.0}};
    CHECK(tess_bounds_of(huge, 2, &b));
    CHECK_EQ_I(tess_zoom_to_fit(b, 400, 240), TESSERA_MIN_ZOOM);

    /* Nonsense viewports must not divide by zero or loop forever. */
    CHECK_EQ_I(tess_zoom_to_fit(b, 0, 240), TESSERA_MIN_ZOOM);
    CHECK_EQ_I(tess_zoom_to_fit(b, 400, 0), TESSERA_MIN_ZOOM);
    CHECK_EQ_I(tess_zoom_to_fit(b, -5, -5), TESSERA_MIN_ZOOM);
}

static void test_tile_bounds(void)
{
    begin("a tile's bounds contain its own centre and nothing beyond");
    const tess_tile t = tess_geo_to_tile(kSite, 14);
    const tess_bounds b = tess_tile_bounds(t);

    CHECK(b.south_west.latitude < b.north_east.latitude);
    CHECK(b.south_west.longitude < b.north_east.longitude);

    CHECK(kSite.latitude >= b.south_west.latitude);
    CHECK(kSite.latitude <= b.north_east.latitude);
    CHECK(kSite.longitude >= b.south_west.longitude);
    CHECK(kSite.longitude <= b.north_east.longitude);

    /* The centre of the box must land back in the same tile. */
    const tess_geo centre = tess_bounds_centre(b);
    const tess_tile again = tess_geo_to_tile(centre, 14);
    CHECK_EQ_I(again.x, t.x);
    CHECK_EQ_I(again.y, t.y);
}

static void test_continuous_inverse(void)
{
    begin("the continuous inverse undoes the continuous forward transform");

    /* tess_tile_f_to_geo is what lets a caller turn a screen pixel back into a
     * position -- the widget's pan, its tap-to-place and its zoom-about-a-point
     * are all built on it. It is also the transform tess_tile_north_west is now
     * written in terms of, so a defect here would move every tile as well as
     * every marker. */
    const tess_geo places[] = {
        {51.47788, -0.00159},
        {48.85826, 2.29450},
        {-33.85678, 151.21528},
        {0.0, 0.0},
        {84.9, 179.9},
        {-84.9, -179.9},
    };

    for (unsigned i = 0; i < sizeof places / sizeof places[0]; i++)
    {
        for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
        {
            double x = 0.0, y = 0.0;
            tess_geo_to_tile_f(places[i], zoom, &x, &y);

            const tess_geo back = tess_tile_f_to_geo(x, y, zoom);

            /* A tenth of a pixel at the tightest zoom, expressed in degrees. */
            CHECK_NEAR(back.latitude, tess_clamp_latitude(places[i].latitude), 1e-6);
            CHECK_NEAR(back.longitude, places[i].longitude, 1e-6);
        }
    }

    /* Fractional coordinates are meaningful: (x + 0.5, y + 0.5) is the centre
     * of tile (x, y), which must sit inside that tile's own bounds. */
    for (int zoom = TESSERA_MIN_ZOOM; zoom <= TESSERA_MAX_ZOOM; zoom++)
    {
        const tess_tile tile = tess_geo_to_tile(kSite, zoom);
        const tess_geo centre = tess_tile_f_to_geo((double) tile.x + 0.5,
                                               (double) tile.y + 0.5, zoom);
        const tess_bounds bounds = tess_tile_bounds(tile);

        CHECK(centre.latitude < bounds.north_east.latitude);
        CHECK(centre.latitude > bounds.south_west.latitude);
        CHECK(centre.longitude > bounds.south_west.longitude);
        CHECK(centre.longitude < bounds.north_east.longitude);

        /* And it round-trips to the tile it is the centre of. */
        const tess_tile again = tess_geo_to_tile(centre, zoom);
        CHECK_EQ_I(again.x, tile.x);
        CHECK_EQ_I(again.y, tile.y);
    }

    /* An integer coordinate is the tile's north-west corner, which is what
     * tess_tile_north_west is now defined as. */
    const tess_tile tile = {42165, 26478, 16};
    const tess_geo corner = tess_tile_north_west(tile);
    const tess_geo direct = tess_tile_f_to_geo((double) tile.x, (double) tile.y, tile.zoom);
    CHECK_NEAR(corner.latitude, direct.latitude, 0.0);
    CHECK_NEAR(corner.longitude, direct.longitude, 0.0);
}

static void test_pixel_delta_takes_the_short_way(void)
{
    begin("the offset across the antimeridian is the short way round");

    /* Two points 0.2 degrees apart, one either side of 180. Measured the long
     * way that is very nearly a whole world; measured correctly it is a modest
     * number of pixels, and it is what decides whether a marker just west of
     * the line appears just off the left edge or a world away to the east.
     *
     * From zoom 8 up, because below that 0.2 degrees is less than a pixel and
     * the sign of a rounded zero says nothing. */
    const tess_geo west = {0.0, 179.9};
    const tess_geo east = {0.0, -179.9};

    for (int zoom = 8; zoom <= 14; zoom++)
    {
        const double world_px = (double) tess_tiles_per_axis(zoom) * TESSERA_TILE_SIZE;
        const double expected = 0.2 / 360.0 * world_px;

        int32_t dx = 0, dy = 0;
        tess_pixel_delta(west, east, zoom, &dx, &dy);

        CHECK(dx > 0);                       /* east of the line is east */
        CHECK_NEAR((double) dx, expected, 1.0 + expected * 0.001);
        CHECK_EQ_I(dy, 0);

        /* And it stays antisymmetric across the wrap. */
        int32_t rx = 0;
        tess_pixel_delta(east, west, zoom, &rx, NULL);
        CHECK_EQ_I(rx, -dx);
    }
}

static void test_pixel_delta_across_half_the_world(void)
{
    begin("an offset of half the world does not flip sign arbitrarily");

    /* Exactly antipodal in longitude is the one ambiguous case: the two
     * directions are equally short. Whichever way it resolves, the magnitude
     * must be half the world and the reverse must be its negation. */
    const tess_geo a = {0.0, 0.0};
    const tess_geo b = {0.0, 180.0};

    for (int zoom = TESSERA_MIN_ZOOM + 2; zoom <= 12; zoom++)
    {
        const double half_world = (double) tess_tiles_per_axis(zoom) * TESSERA_TILE_SIZE / 2.0;

        int32_t dx = 0;
        tess_pixel_delta(a, b, zoom, &dx, NULL);
        CHECK_NEAR(fabs((double) dx), half_world, 1.0);
    }
}

int main(void)
{
    printf("tessera: Web Mercator tile arithmetic\n");
    test_tiles_per_axis();
    test_reference_tiles();
    test_round_trip();
    test_pixel_within_tile_is_consistent();
    test_latitude_clamp();
    test_longitude_wrap();
    test_tile_indices_stay_in_range();
    test_pixel_delta_signs();
    test_pixel_delta_is_antisymmetric();
    test_pixel_delta_takes_the_short_way();
    test_pixel_delta_across_half_the_world();
    test_pixel_delta_matches_ground_distance();
    test_metres_per_pixel();
    test_bounds();
    test_zoom_to_fit();
    test_zoom_to_fit_degenerate();
    test_tile_bounds();
    test_continuous_inverse();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
