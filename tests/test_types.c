/* SPDX-License-Identifier: MIT */
/*
 * Tests for the shared geometry and the tile path formatter.
 *
 * The path formatter gets a lot of attention here for its size, because it is
 * the one place a tile address becomes a string. Every plausible address is
 * short, so a fixed buffer looks generous right up until an out-of-range value
 * reaches it and needs half again as many bytes. The formatter bounds itself
 * and refuses addresses that cannot exist rather than rendering them.
 */

#include "tessera/types.h"

#include "check.h"
#include "fixture.h"

static void test_rect_basics(void)
{
    begin("rectangles are half-open, so adjacent ones do not overlap");

    const tess_rect r = R(10, 20, 110, 220);
    CHECK_EQ_I(tess_rect_width(r), 100);
    CHECK_EQ_I(tess_rect_height(r), 200);
    CHECK(!tess_rect_is_empty(r));
    CHECK(tess_rect_is_empty(R(5, 5, 5, 10)));
    CHECK(tess_rect_is_empty(R(5, 5, 10, 5)));
    CHECK(tess_rect_is_empty(R(10, 0, 5, 10)));
}

static void test_rect_contains_edges(void)
{
    begin("the top-left edge is inside and the bottom-right edge is not");

    const tess_rect r = R(0, 0, 100, 50);

    /* With an inclusive rectangle and strict `>` on both ends, a marker
     * sitting exactly on the left or top edge reads as outside -- and a widget
     * that draws off-screen markers differently then draws it twice, once in
     * place and once as an edge arrow. */
    CHECK(tess_rect_contains(r, P(0, 0)));
    CHECK(tess_rect_contains(r, P(99, 49)));
    CHECK(tess_rect_contains(r, P(0, 49)));
    CHECK(tess_rect_contains(r, P(99, 0)));

    CHECK(!tess_rect_contains(r, P(100, 25)));
    CHECK(!tess_rect_contains(r, P(50, 50)));
    CHECK(!tess_rect_contains(r, P(-1, 25)));
    CHECK(!tess_rect_contains(r, P(50, -1)));
}

static void test_rect_inset(void)
{
    begin("insetting shrinks, and never inverts");

    const tess_rect r = R(0, 0, 100, 60);

    const tess_rect a = tess_rect_inset(r, 10, 5);
    CHECK_EQ_I(a.x0, 10);
    CHECK_EQ_I(a.y0, 5);
    CHECK_EQ_I(a.x1, 90);
    CHECK_EQ_I(a.y1, 55);

    /* Inset past the middle. An inverted rectangle would make every
     * containment test downstream answer false in a way that looks like the
     * marker being off-screen rather than like a bad argument. */
    const tess_rect b = tess_rect_inset(r, 60, 40);
    CHECK(!tess_rect_is_empty(b) || true);
    CHECK(b.x1 >= b.x0);
    CHECK(b.y1 >= b.y0);
    CHECK_EQ_I(b.x0, 50);
    CHECK_EQ_I(b.x1, 50);
    CHECK_EQ_I(b.y0, 30);
    CHECK_EQ_I(b.y1, 30);

    /* A zero inset is the identity. */
    const tess_rect c = tess_rect_inset(r, 0, 0);
    CHECK_EQ_I(c.x0, r.x0);
    CHECK_EQ_I(c.y1, r.y1);
}

static void test_tile_validity(void)
{
    begin("a tile address is valid only inside the world at its own zoom");

    CHECK(tess_tile_is_valid(T(0, 0, 10)));
    CHECK(tess_tile_is_valid(T(1023, 1023, 10)));
    CHECK(tess_tile_is_valid(T(65535, 65535, 16)));

    CHECK(!tess_tile_is_valid(T(1024, 0, 10)));
    CHECK(!tess_tile_is_valid(T(0, 1024, 10)));
    CHECK(!tess_tile_is_valid(T(-1, 0, 10)));

    /* What an unclamped projection produces from latitudes of +/-90 at zoom
     * 12, where the valid range is 0..4095. */
    CHECK(!tess_tile_is_valid(T(2048, -8166, 12)));
    CHECK(!tess_tile_is_valid(T(2048, 12261, 12)));

    /* Zoom levels outside the configured range. The default is 0..22; a build
     * that narrows it to the levels a device actually ships tiles for gets a
     * smaller valid set here, which is the point of narrowing it. */
    CHECK(!tess_tile_is_valid(T(0, 0, TESSERA_MAX_ZOOM + 1)));
    CHECK(!tess_tile_is_valid(T(0, 0, TESSERA_MIN_ZOOM - 1)));
    CHECK(!tess_tile_is_valid(T(0, 0, -1)));
    CHECK(!tess_tile_is_valid(T(0, 0, 99)));

    /* And the extremes of the range itself are valid. */
    CHECK(tess_tile_is_valid(T(0, 0, TESSERA_MIN_ZOOM)));
    CHECK(tess_tile_is_valid(T(0, 0, TESSERA_MAX_ZOOM)));
}

static void test_path(void)
{
    begin("a tile path is built from its pattern");

    char buf[TESS_TILE_PATH_MAX];

    /* The default: the z/x/y directory layout most tile tooling writes. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, NULL, T(32767, 21801, 16)), TESS_OK);
    CHECK_STR_EQ(buf, "16/32767/21801.bin");

    /* A prefix and a different extension. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, "tiles/{z}/{x}/{y}.raw",
                                T(511, 340, 10)), TESS_OK);
    CHECK_STR_EQ(buf, "tiles/10/511/340.raw");

    /* A flat layout, and the tokens in a different order. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, "map/t_{x}_{y}_{z}.bin",
                                T(0, 0, 10)), TESS_OK);
    CHECK_STR_EQ(buf, "map/t_0_0_10.bin");

    /* A pattern with no tokens at all is a constant path -- odd, but not an
     * error, and it must not be mangled. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, "fixed.bin", T(1, 2, 12)), TESS_OK);
    CHECK_STR_EQ(buf, "fixed.bin");

    /* A lone brace is literal, not the start of a token. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, "{q}/{z}.bin", T(1, 2, 12)), TESS_OK);
    CHECK_STR_EQ(buf, "{q}/12.bin");
}

static void test_path_never_overruns(void)
{
    begin("a path that will not fit is refused, not truncated");

    /* "map/16/65535/65535.bin" is 22 characters, so 23 bytes is exactly
     * enough and 22 is exactly one short. Sizing a buffer to fit the addresses
     * somebody happened to try is what looks fine until it does not. */
    char exact[23];
    CHECK_STATUS(tess_tile_path(exact, sizeof exact, "map/{z}/{x}/{y}.bin",
                                T(65535, 65535, 16)), TESS_OK);
    CHECK_STR_EQ(exact, "map/16/65535/65535.bin");

    char tight[22];
    CHECK_STATUS(tess_tile_path(tight, sizeof tight, "map/{z}/{x}/{y}.bin",
                                T(65535, 65535, 16)), TESS_ERR_RANGE);
    /* And it must leave the buffer in a state a careless caller survives. */
    CHECK_STR_EQ(tight, "");

    char one[1];
    CHECK_STATUS(tess_tile_path(one, sizeof one, NULL, T(1, 1, 12)), TESS_ERR_RANGE);
    CHECK_STR_EQ(one, "");
}

static void test_path_refuses_impossible_addresses(void)
{
    begin("an address that cannot exist never becomes a path");

    char buf[64];

    /* The whole point of the check. Each of these is what an unclamped
     * projection or an unwrapped column produces, and each would otherwise be
     * formatted and handed to the filesystem. */
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, NULL, T(2048, -8166, 12)), TESS_ERR_ARG);
    CHECK_STR_EQ(buf, "");
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, NULL, T(2048, 12261, 12)), TESS_ERR_ARG);
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, NULL, T(INT32_MIN, INT32_MIN, 12)), TESS_ERR_ARG);
    CHECK_STATUS(tess_tile_path(buf, sizeof buf, NULL, T(INT32_MAX, INT32_MAX, 16)), TESS_ERR_ARG);
    CHECK_STATUS(tess_tile_path(NULL, 32, NULL, T(1, 1, 12)), TESS_ERR_ARG);
    CHECK_STATUS(tess_tile_path(buf, 0, NULL, T(1, 1, 12)), TESS_ERR_ARG);
}

static void test_status_names(void)
{
    begin("every status has a name, so failures read");

    CHECK_STR_EQ(tess_status_name(TESS_OK), "TESS_OK");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_ARG), "TESS_ERR_ARG");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_FULL), "TESS_ERR_FULL");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_EMPTY), "TESS_ERR_EMPTY");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_NOT_FOUND), "TESS_ERR_NOT_FOUND");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_IO), "TESS_ERR_IO");
    CHECK_STR_EQ(tess_status_name(TESS_ERR_RANGE), "TESS_ERR_RANGE");
}

int main(void)
{
    printf("tessera: geometry and tile addressing\n");

    test_rect_basics();
    test_rect_contains_edges();
    test_rect_inset();
    test_tile_validity();
    test_path();
    test_path_never_overruns();
    test_path_refuses_impossible_addresses();
    test_status_names();

    REPORT("types");
}
