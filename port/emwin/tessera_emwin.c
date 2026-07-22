/* SPDX-License-Identifier: MIT */

#include "tessera_emwin.h"

#include <string.h>

#define TESSERA_TIMER_ID (GUI_ID_USER + 0x0E1)

/*
 * Per-window state, held in the window's extra bytes.
 *
 * emWin's user data is a copy in and a copy out, so this struct is kept small
 * -- a config, a timer handle and three integers of drag state, around sixty
 * bytes -- and the engine it drives is referenced by pointer rather than
 * stored here.
 *
 * Keeping the map itself in user data would be the tempting shortcut, and it
 * scales badly in a way that is invisible until it is not: every field access
 * becomes a get / modify / set round trip over the whole structure, and a
 * repaint makes several of those.
 */
typedef struct
{
    TESSERA_CONFIG config;
    WM_HTIMER timer;

    /* Drag state for touch panning. */
    bool dragging;
    int last_x;
    int last_y;
} TESSERA_STATE;

static bool state_get(WM_HWIN window, TESSERA_STATE *out)
{
    return WM_GetUserData(window, out, (int) sizeof(*out)) == (int) sizeof(*out);
}

static void state_put(WM_HWIN window, const TESSERA_STATE *in)
{
    (void) WM_SetUserData(window, in, (int) sizeof(*in));
}

/* ------------------------------------------------------------- the painter */

typedef struct
{
    const TESSERA_STATE *state;
    int origin_x;  /* window origin in screen coordinates */
    int origin_y;
} paint_ctx;

static void paint_tile(void *ctx, const void *image, tess_tile tile, tess_point origin)
{
    paint_ctx *pc = (paint_ctx *) ctx;
    GUI_BITMAP bitmap;
    GUI_LOGPALETTE palette;

    (void) tile;

    memset(&bitmap, 0, sizeof(bitmap));
    memset(&palette, 0, sizeof(palette));

    /*
     * Parse the stream header on each paint rather than caching a GUI_BITMAP
     * per slot.
     *
     * It is a few dozen bytes of header parsing against a 128 KB blit, so the
     * cost does not show up -- and it means a cache slot holds nothing but the
     * bytes that came off the medium. Anything derived from those bytes and
     * stored beside them is a second thing to keep in step, and it would have
     * to be built by the loader thread and read by the drawing one.
     */
    GUI_CreateBitmapFromStreamA565(&bitmap, &palette, image);

    GUI_DrawBitmap(&bitmap, pc->origin_x + origin.x, pc->origin_y + origin.y);
}

static void paint_placeholder(void *ctx, tess_tile tile, tess_point origin)
{
    paint_ctx *pc = (paint_ctx *) ctx;
    const int x = pc->origin_x + origin.x;
    const int y = pc->origin_y + origin.y;

    (void) tile;

    GUI_SetBkColor(pc->state->config.placeholder_bk);
    GUI_SetColor(pc->state->config.placeholder_fg);
    GUI_ClearRect(x, y, x + TESSERA_TILE_SIZE - 1, y + TESSERA_TILE_SIZE - 1);
    GUI_DrawRect(x, y, x + TESSERA_TILE_SIZE - 1, y + TESSERA_TILE_SIZE - 1);

    if (pc->state->config.placeholder_font != NULL)
    {
        GUI_SetFont(pc->state->config.placeholder_font);
    }
    GUI_DispStringHCenterAt("No tile", x + TESSERA_TILE_SIZE / 2,
                            y + TESSERA_TILE_SIZE / 2);
}

/*
 * Draw `source` rotated by `degrees` clockwise, centred on (cx, cy).
 *
 * Rotation in emWin goes through a pair of memory devices; there is no
 * lighter path. They are created and destroyed around a single marker rather
 * than held open, so the peak cost is two devices however many markers there
 * are.
 *
 * Returns false when it could not rotate, so the caller can fall back to
 * drawing the bitmap upright. GUI_MEMDEV_CreateFixed returns 0 on a full emWin
 * heap, and a repaint that ignores that turns a low-memory condition into a
 * null dereference.
 */
static bool draw_rotated(const TESSERA_STATE *state, const GUI_BITMAP *source,
                         int cx, int cy, int degrees)
{
    const int size = state->config.rotation_size;

    if (source == NULL || state->config.rotation_buffer == NULL || size <= 0)
    {
        return false;
    }

    GUI_MEMDEV_Handle src = GUI_MEMDEV_CreateFixed(0, 0, size, size, GUI_MEMDEV_NOTRANS,
                                                   GUI_MEMDEV_APILIST_32, GUICC_8888);
    if (src == 0)
    {
        return false;
    }

    GUI_MEMDEV_Handle dst = GUI_MEMDEV_CreateFixed(0, 0, size, size, GUI_MEMDEV_NOTRANS,
                                                   GUI_MEMDEV_APILIST_32, GUICC_8888);
    if (dst == 0)
    {
        GUI_MEMDEV_Delete(src);
        return false;
    }

    GUI_MEMDEV_Select(src);
    GUI_SetBkColor(GUI_TRANSPARENT);
    GUI_Clear();
    /* Centre the artwork in the square so that rotation turns it about its own
     * middle. Drawing it anywhere else puts the pivot off the glyph, and the
     * marker then orbits the centre instead of spinning in place. */
    GUI_DrawBitmap(source, (size - source->XSize) / 2, (size - source->YSize) / 2);

    GUI_MEMDEV_Select(dst);
    GUI_SetBkColor(GUI_TRANSPARENT);
    GUI_Clear();
    GUI_MEMDEV_RotateHQ(src, dst, 0, 0, degrees * 1000, 1000);

    GUI_MEMDEV_Select(0);
    GUI_MEMDEV_WriteAt(dst, cx - size / 2, cy - size / 2);

    GUI_MEMDEV_Delete(src);
    GUI_MEMDEV_Delete(dst);
    return true;
}

static void draw_bitmap_centred(const GUI_BITMAP *bitmap, int cx, int cy)
{
    if (bitmap != NULL)
    {
        GUI_DrawBitmap(bitmap, cx - bitmap->XSize / 2, cy - bitmap->YSize / 2);
    }
    else
    {
        /* No artwork configured: a filled disc, so a board can be brought up
         * before the icons exist. */
        GUI_FillCircle(cx, cy, 5);
    }
}

static void paint_marker(void *ctx, const tess_marker *marker, tess_marker_placement placement,
                         uint8_t index)
{
    paint_ctx *pc = (paint_ctx *) ctx;
    const TESSERA_CONFIG *config = &pc->state->config;

    const int cx = pc->origin_x + (int) placement.point.x;
    const int cy = pc->origin_y + (int) placement.point.y;

    if (!placement.on_screen)
    {
        /* An arrow on the edge, turned to point at where the marker actually
         * is. tess_marker_locate has already put it on the inset rectangle, so
         * the whole glyph is on screen. */
        GUI_SetColor(GUI_CYAN);
        if (!draw_rotated(pc->state, config->arrow, cx, cy, placement.bearing_deg))
        {
            draw_bitmap_centred(config->arrow, cx, cy);
        }
        return;
    }

    if (index == TESS_MARKER_FOCUS && marker->has_heading)
    {
        GUI_SetColor(GUI_RED);
        if (!draw_rotated(pc->state, config->focus, cx, cy, marker->heading_deg))
        {
            draw_bitmap_centred(config->focus, cx, cy);
        }
        return;
    }

    GUI_SetColor(GUI_YELLOW);
    draw_bitmap_centred((index == TESS_MARKER_FOCUS) ? config->focus : config->pin, cx, cy);
}

/* ------------------------------------------------------------- the callback */

static void on_paint(WM_MESSAGE *message, const TESSERA_STATE *state)
{
    paint_ctx ctx;
    ctx.state = state;
    /* emWin's drawing calls are in screen coordinates while the engine works
     * in widget-relative ones, so the window origin is added once here rather
     * than at every call site. */
    ctx.origin_x = WM_GetWindowOrgX(message->hWin);
    ctx.origin_y = WM_GetWindowOrgY(message->hWin);

    tess_painter painter;
    memset(&painter, 0, sizeof(painter));
    painter.ctx = &ctx;
    painter.draw_tile = paint_tile;
    painter.draw_placeholder = paint_placeholder;
    painter.draw_marker = paint_marker;

    (void) tess_map_draw(state->config.map, &painter);
}

static void on_touch(WM_MESSAGE *message, TESSERA_STATE *state)
{
    const GUI_PID_STATE *pid = (const GUI_PID_STATE *) message->Data.p;

    if (!state->config.touch_pan)
    {
        return;
    }

    /* A NULL payload means the pointer left the window. Ending the drag is
     * what stops the map from jumping the next time a finger lands somewhere
     * else entirely. */
    if (pid == NULL || !pid->Pressed)
    {
        state->dragging = false;
        return;
    }

    if (!state->dragging)
    {
        state->dragging = true;
        state->last_x = pid->x;
        state->last_y = pid->y;
        return;
    }

    const int dx = pid->x - state->last_x;
    const int dy = pid->y - state->last_y;

    state->last_x = pid->x;
    state->last_y = pid->y;

    if (dx == 0 && dy == 0)
    {
        return;
    }

    /* Dragging right moves the content right, which means the view moves
     * *west* -- so the sign is inverted relative to the finger. */
    tess_map_pan(state->config.map, -dx, -dy);
    WM_InvalidateWindow(message->hWin);
}

void TESSERA_Callback(WM_MESSAGE *message)
{
    TESSERA_STATE state;

    /* A window whose user data is not ours -- which includes every message
     * that arrives before TESSERA_CreateEx has finished setting it -- goes to
     * the default handler rather than being interpreted as state. */
    if (!state_get(message->hWin, &state) || state.config.map == NULL)
    {
        WM_DefaultProc(message);
        return;
    }

    switch (message->MsgId)
    {
    case WM_PAINT:
        on_paint(message, &state);
        break;

    case WM_TOUCH:
        on_touch(message, &state);
        state_put(message->hWin, &state);  /* the drag state changed */
        break;

    case WM_TIMER:
        /* Repaint so tiles appear as the loader delivers them, and restart
         * the timer. Nothing else belongs in here: a widget that broadcasts to
         * the application from its own timer, or asks which screen is
         * currently visible, has coupled itself to the navigation for no
         * benefit. */
        WM_RestartTimer(state.timer, state.config.refresh_ms);
        if (tess_map_pending(state.config.map) > 0)
        {
            /* Invalidate only while something is still arriving. Repainting
             * on every tick whether or not anything changed is a screenful of
             * blits per tick that nobody asked for, and on a small part those
             * blits compete with the loader for the memory bus. */
            WM_InvalidateWindow(message->hWin);
        }
        break;

    case WM_DELETE:
        if (state.timer != 0)
        {
            WM_DeleteTimer(state.timer);
            state.timer = 0;
            state_put(message->hWin, &state);
        }
        break;

    default:
        WM_DefaultProc(message);
        break;
    }
}

/* ----------------------------------------------------------------- creation */

WM_HWIN TESSERA_CreateEx(const TESSERA_CONFIG *config, WM_HWIN parent,
                         int x0, int y0, int width, int height, int id)
{
    if (config == NULL || config->map == NULL || width <= 0 || height <= 0)
    {
        return 0;
    }

    /* Keep the engine's idea of the viewport and emWin's the same size. A
     * mismatch is invisible until a marker near an edge is placed against the
     * wrong rectangle. */
    if (tess_map_set_size(config->map, width, height) != TESS_OK)
    {
        return 0;
    }

    /* WM_CF_MEMDEV: emWin composes the frame off-screen and blits it once, so
     * the map does not visibly build up tile by tile. The extra bytes hold the
     * state struct, so it is freed with the window and there is no second
     * lifetime to get wrong. */
    const WM_HWIN window = WM_CreateWindowAsChild(
        x0, y0, width, height, parent,
        WM_CF_SHOW | WM_CF_MEMDEV,
        TESSERA_Callback,
        (int) sizeof(TESSERA_STATE));

    if (window == 0)
    {
        return 0;
    }

    TESSERA_STATE state;
    memset(&state, 0, sizeof(state));
    state.config = *config;
    if (state.config.refresh_ms <= 0)
    {
        state.config.refresh_ms = 100;
    }

    /* The state has to be in place before the timer exists, or the first
     * WM_TIMER could arrive at a callback that reads uninitialised bytes. */
    state_put(window, &state);

    state.timer = WM_CreateTimer(window, TESSERA_TIMER_ID, state.config.refresh_ms, 0);
    state_put(window, &state);

    WM_SetId(window, id);
    WM_InvalidateWindow(window);
    return window;
}

tess_map *TESSERA_GetMap(WM_HWIN window)
{
    TESSERA_STATE state;
    return state_get(window, &state) ? state.config.map : NULL;
}
