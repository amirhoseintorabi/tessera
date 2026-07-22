/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_MARKER_H
#define TESSERA_MARKER_H

/*
 * Markers -- whatever the map is following, a destination, a point of
 * interest -- and where to draw one that
 * has gone off the edge of the viewport.
 *
 * The off-screen case is the interesting half. When a marker is outside the
 * view, the widget shows an arrow pinned to the edge, pointing at it, so the
 * user knows which way to pan. Getting that placement right is a ray-rectangle
 * intersection: take the ray from the viewport centre towards the marker and
 * find where it leaves the rectangle.
 *
 * The obvious approach -- split the rectangle into four triangular regions by
 * its diagonals and write a formula per region -- needs four formulas that
 * must agree at three boundaries each, plus special cases for a marker exactly
 * on a diagonal and one exactly at the centre. The version here is a single
 * scale factor with no case analysis at all: six lines instead of sixty, and
 * the corners and diagonals fall out of it rather than being handled.
 */

#include "tessera/types.h"
#include "tessera/view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Label length, including the terminator. Enough for a street name or a short
 * place name; anything longer is truncated rather than refused. */
#ifndef TESS_MARKER_LABEL_MAX
#define TESS_MARKER_LABEL_MAX 30
#endif

typedef struct
{
    tess_geo position;
    /*
     * Heading in compass degrees: 0 north, 90 east, increasing clockwise, in
     * [0, 360). Only meaningful when `has_heading` is set -- a pickup point
     * has a position but no orientation, and drawing it rotated to an
     * arbitrary leftover value is worse than drawing it upright.
     */
    int16_t heading_deg;
    bool has_heading;
    bool visible;
    char label[TESS_MARKER_LABEL_MAX];
} tess_marker;

typedef struct
{
    /* True when the marker is inside the viewport, so it is drawn as itself at
     * `point`. False when it is outside, so an arrow is drawn at `point` --
     * which is then on the inset edge -- rotated to `bearing_deg`. */
    bool on_screen;
    tess_point point;
    /* Direction from the viewport centre to the marker, in compass degrees.
     * Meaningful in both cases; the arrow uses it, and a caller may want it
     * on-screen too, for a "300 m, north-east" readout. */
    int16_t bearing_deg;
} tess_marker_placement;

/* Set a marker's position, clearing its heading. Safe with a NULL label. */
void tess_marker_set(tess_marker *marker, tess_geo position, const char *label);

/* Set the heading, normalised into [0, 360). */
void tess_marker_set_heading(tess_marker *marker, int heading_deg);

/*
 * Where to draw `position` given `view`.
 *
 * `edge_inset` is how far in from the viewport edge an off-screen arrow sits,
 * and should be about half the arrow bitmap's size so that the whole glyph
 * stays on screen. The containment test uses the same inset rectangle, so a
 * marker never falls into the gap between "too close to the edge to draw whole"
 * and "far enough out to be an arrow".
 *
 * A marker exactly at the viewport centre has no direction; it reports
 * on-screen at the centre with a bearing of 0.
 */
tess_marker_placement tess_marker_locate(const tess_view *view, tess_geo position, int32_t edge_inset);

/*
 * Compass bearing of the screen offset (dx, dy), in [0, 360).
 *
 * Screen y grows downwards while north is up, so this is atan2(dx, -dy) and
 * not the atan2(dy, dx) that a reader expects. Exposed because the rotation
 * applied to a marker bitmap has to use the same convention, and having two
 * places derive it independently is how the sign gets flipped.
 */
int16_t tess_bearing_from_offset(int32_t dx, int32_t dy);

/* Normalise any degree value into [0, 360). */
int16_t tess_normalise_degrees(int degrees);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_MARKER_H */
