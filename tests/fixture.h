/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_TESTS_FIXTURE_H
#define TESSERA_TESTS_FIXTURE_H

/*
 * Values and constructors the test suites share.
 *
 * Everything here is `static inline` so that a suite which uses none of it
 * still compiles warning-free -- a shared fixture header is only worth having
 * if including it costs nothing.
 */

#include "tessera/types.h"

/*
 * A fixed reference position for tests that need somewhere to stand: the Royal
 * Observatory, Greenwich.
 *
 * Chosen because it is public, unambiguous, and sits within a thousandth of a
 * degree of the prime meridian -- so a longitude sign error shows up as a
 * change of hemisphere rather than as a plausible-looking number.
 */
static inline tess_geo test_site(void)
{
    const tess_geo site = {51.47788, -0.00159};
    return site;
}

/* Brace-free constructors, so a table of cases stays readable. */

static inline tess_tile T(int32_t x, int32_t y, int32_t zoom)
{
    const tess_tile tile = {x, y, zoom};
    return tile;
}

static inline tess_point P(int32_t x, int32_t y)
{
    const tess_point point = {x, y};
    return point;
}

static inline tess_rect R(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    const tess_rect rect = {x0, y0, x1, y1};
    return rect;
}

#endif /* TESSERA_TESTS_FIXTURE_H */
