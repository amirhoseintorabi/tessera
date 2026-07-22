/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_INTEROP_EMWIN_PROBE_H
#define TESSERA_INTEROP_EMWIN_PROBE_H

/*
 * The recording side of the emWin interoperability layer.
 *
 * The GUI_* and WM_* declarations next door are implemented here as counters,
 * so a test can assert that the binding drew twenty-five tiles and two
 * markers, balanced its memory devices and restarted its timer -- with no
 * emWin present at all.
 *
 * This header is not part of the port. Nothing in ../ includes it, and it does
 * not exist when building against a licensed emWin.
 */

#include "tessera_emwin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t bitmaps_drawn;      /* GUI_DrawBitmap                         */
    uint32_t streams_parsed;     /* GUI_CreateBitmapFromStreamA565         */
    uint32_t rects_cleared;      /* GUI_ClearRect                          */
    uint32_t strings_drawn;      /* GUI_DispStringHCenterAt                */
    uint32_t circles_drawn;      /* GUI_FillCircle                         */
    uint32_t memdevs_created;
    uint32_t memdevs_deleted;
    uint32_t rotations;
    uint32_t memdev_writes;

    uint32_t windows_created;
    uint32_t windows_deleted;
    uint32_t invalidations;
    uint32_t timers_created;
    uint32_t timers_restarted;
    uint32_t timers_deleted;
    uint32_t default_proc_calls;

    /* Where the last bitmap went, so a test can check placement. */
    int last_bitmap_x;
    int last_bitmap_y;

    /* Highest number of memory devices alive at once. The widget should never
     * hold more than two: it rotates one marker at a time. */
    uint32_t peak_memdevs;
} tess_emwin_probe;

/* Tear everything down: windows, timers, memory devices and counters. Any
 * WM_HWIN obtained before this becomes invalid. */
void tess_emwin_probe_reset(void);

/* Zero the counters only, leaving the windows alone -- for a test that wants
 * to measure one frame of an already-created widget. Keeping this separate
 * from tess_emwin_probe_reset matters: a reset that also destroyed the window
 * would make the next message go nowhere, and the test would read zero
 * everything and call it a pass. */
void tess_emwin_probe_reset_counters(void);

tess_emwin_probe tess_emwin_probe_stats(void);

/* Deliver a message to a window's callback, the way emWin's message pump
 * would. `data` becomes message->Data.p. */
void tess_emwin_probe_send(WM_HWIN window, int message_id, const void *data);

/* Place a window at a screen position, so that the origin arithmetic in the
 * widget's painter is exercised with a non-zero offset. */
void tess_emwin_probe_set_origin(WM_HWIN window, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_INTEROP_EMWIN_PROBE_H */
