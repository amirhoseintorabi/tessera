/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PORT_EMWIN_H
#define TESSERA_PORT_EMWIN_H

/*
 * The SEGGER emWin binding.
 *
 * A custom emWin window that draws a Tessera map: a WM callback, a painter
 * built from GUI_* calls, drag-to-pan on WM_TOUCH, and a timer that repaints
 * as tiles arrive. Everything about *what* to draw lives in the engine and is
 * covered by its own tests; this file is only the translation into emWin, and
 * is kept small enough to read in one sitting for that reason.
 *
 * Two design points worth knowing before reading the source, because both are
 * departures from how an emWin composite widget is usually written:
 *
 *   No child widgets. It would be natural to create an IMAGE per tile position
 *   and pan by moving them, but that means one window object per tile plus the
 *   invalidation traffic to match, and it means the window positions and the
 *   map's idea of where things are can drift apart. Drawing the whole map in
 *   WM_PAINT keeps a single source of truth for position and costs nothing:
 *   the blits happen either way.
 *
 *   One rotation buffer, not one per marker. Painting is synchronous, so only
 *   one marker is ever being rotated at a time. A persistent rotated bitmap
 *   per marker would be ten buffers where one will do, and would have to be
 *   invalidated whenever a heading changed.
 *
 * emWin is commercially licensed by SEGGER. This repository contains no emWin
 * source, headers or binaries. This file calls the published API; CI compiles
 * it against the independent interoperability declarations in `interop/`, so
 * that it is built and exercised rather than merely written. To build for a
 * real target, put a licensed emWin on the include path ahead of `interop/` --
 * the includes below are by plain name for exactly that reason. See
 * interop/README.md.
 */

/*
 * Against a licensed emWin these are SEGGER's own headers, by their own names.
 * Define TESSERA_EMWIN_INTEROP to build instead against the hand-written
 * declarations in interop/ -- which is what CI does, so that this binding is
 * compiled and exercised rather than merely written. Nothing in this
 * repository is named GUI.h or WM.h.
 */
#ifdef TESSERA_EMWIN_INTEROP
#include "tessera_emwin_api.h"
#else
#include "GUI.h"
#include "WM.h"
#endif

#include "tessera/map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* The engine. Owned by the caller and must outlive the window; the widget
     * keeps a pointer to it, never a copy. emWin's user data is a copy in and
     * a copy out, and a map's state is far too large to move through it. */
    tess_map *map;

    /*
     * Marker artwork. Any may be NULL, in which case that kind of marker is
     * drawn as a plain filled circle -- enough to bring a board up before the
     * artwork exists.
     *
     * `vehicle` is drawn rotated to the marker's heading, `arrow` rotated to
     * the bearing of an off-screen marker, and `pin` as-is.
     */
    const GUI_BITMAP *vehicle;
    const GUI_BITMAP *pin;
    const GUI_BITMAP *arrow;

    /*
     * Scratch for rotation: at least `rotation_size` x `rotation_size` pixels
     * at 32 bits each. One buffer serves every marker because painting is
     * synchronous -- only one rotation is in progress at a time.
     *
     * NULL disables rotation, and markers are then drawn upright.
     */
    void *rotation_buffer;
    int rotation_size;

    /* Repaint period in milliseconds. The map is repainted on this timer so
     * that tiles appear as the loader delivers them. 100 is a good default:
     * fast enough to feel live, slow enough not to crowd out the loader. */
    int refresh_ms;

    /* Colours for the placeholder drawn where a tile has not arrived. */
    GUI_COLOR placeholder_bk;
    GUI_COLOR placeholder_fg;

    /* Font for the placeholder label. NULL for the current font. */
    const GUI_FONT *placeholder_font;

    /* Enable drag-to-pan. Off by default, because a map embedded as a
     * read-only status panel should not move when the screen is touched. */
    bool touch_pan;
} TESSERA_CONFIG;

/*
 * Create the widget as a child of `parent`.
 *
 * Returns 0 if the window could not be created or the configuration is
 * unusable -- a NULL config or a config with no map.
 */
WM_HWIN TESSERA_CreateEx(const TESSERA_CONFIG *config, WM_HWIN parent,
                         int x0, int y0, int width, int height, int id);

/* The engine behind a widget handle, or NULL. Application code uses this to
 * move markers and the view: TESSERA_GetMap(h) then tess_map_marker_set(...). */
tess_map *TESSERA_GetMap(WM_HWIN window);

/*
 * The widget's message handler.
 *
 * Exposed so that an application which needs its own callback can chain to it
 * rather than reimplement it -- handle what it cares about, and pass the rest
 * here instead of to WM_DefaultProc.
 */
void TESSERA_Callback(WM_MESSAGE *message);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PORT_EMWIN_H */
