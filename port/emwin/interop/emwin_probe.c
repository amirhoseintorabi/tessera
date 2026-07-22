/* SPDX-License-Identifier: MIT */

/*
 * The emWin interoperability declarations, implemented as a recorder.
 *
 * A minimal window manager -- handles, extra bytes, callbacks, timers -- plus
 * drawing calls that count instead of drawing. Enough for the binding to be
 * compiled under the project's warning settings, created, sent messages, and
 * checked on what it did.
 *
 * It reproduces the parts of emWin's contract the binding depends on: user
 * data is a copy in and a copy out and is zeroed at creation, and a callback
 * is invoked with the window handle rather than a pointer to anything.
 *
 * It is not an emWin emulator and does not try to be. It cannot tell anyone
 * whether emWin's clipping, memory-device rotation or bitmap-stream parsing
 * behave as the binding assumes -- that needs the real library on real
 * hardware. See README.md and docs/EMWIN.md.
 */

#include "emwin_probe.h"

#include <stdbool.h>
#include <string.h>

#define MAX_WINDOWS 8
#define MAX_EXTRA_BYTES 512
#define MAX_MEMDEVS 16

const void *GUICC_8888 = "GUICC_8888";
const void *GUI_MEMDEV_APILIST_32 = "GUI_MEMDEV_APILIST_32";
const void *GUI_DRAW_BMPA565 = "GUI_DRAW_BMPA565";

typedef struct
{
    bool in_use;
    WM_CALLBACK *callback;
    int x0, y0, width, height;
    int id;
    uint8_t extra[MAX_EXTRA_BYTES];
    int extra_bytes;
} probe_window;

static probe_window windows[MAX_WINDOWS];
static bool memdevs[MAX_MEMDEVS];
static uint32_t live_memdevs;
static int next_timer = 1;
static tess_emwin_probe record;

/* Window handles are 1-based so that 0 stays the "no window" value emWin
 * uses and TESSERA_CreateEx's failure return is distinguishable. */
static probe_window *window_of(WM_HWIN handle)
{
    if (handle < 1 || handle > MAX_WINDOWS)
    {
        return NULL;
    }
    probe_window *w = &windows[handle - 1];
    return w->in_use ? w : NULL;
}

void tess_emwin_probe_reset(void)
{
    memset(windows, 0, sizeof(windows));
    memset(memdevs, 0, sizeof(memdevs));
    memset(&record, 0, sizeof(record));
    live_memdevs = 0;
    next_timer = 1;
}

void tess_emwin_probe_reset_counters(void)
{
    memset(&record, 0, sizeof(record));
}

tess_emwin_probe tess_emwin_probe_stats(void)
{
    return record;
}

void tess_emwin_probe_send(WM_HWIN window, int message_id, const void *data)
{
    probe_window *w = window_of(window);
    if (w == NULL || w->callback == NULL)
    {
        return;
    }

    WM_MESSAGE message;
    memset(&message, 0, sizeof(message));
    message.MsgId = message_id;
    message.hWin = window;
    message.Data.p = data;

    w->callback(&message);
}

void tess_emwin_probe_set_origin(WM_HWIN window, int x, int y)
{
    probe_window *w = window_of(window);
    if (w != NULL)
    {
        w->x0 = x;
        w->y0 = y;
    }
}

/* ---------------------------------------------------------- window manager */

WM_HWIN WM_CreateWindowAsChild(int x0, int y0, int width, int height, WM_HWIN parent,
                               uint32_t style, WM_CALLBACK *callback, int extra_bytes)
{
    (void) parent;
    (void) style;

    if (extra_bytes > MAX_EXTRA_BYTES)
    {
        return 0;  /* the real thing would fail on a full emWin heap */
    }

    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (windows[i].in_use)
        {
            continue;
        }

        memset(&windows[i], 0, sizeof(windows[i]));
        windows[i].in_use = true;
        windows[i].callback = callback;
        windows[i].x0 = x0;
        windows[i].y0 = y0;
        windows[i].width = width;
        windows[i].height = height;
        windows[i].extra_bytes = extra_bytes;

        record.windows_created++;
        return i + 1;
    }
    return 0;
}

void WM_DeleteWindow(WM_HWIN window)
{
    probe_window *w = window_of(window);
    if (w == NULL)
    {
        return;
    }

    /* emWin sends WM_DELETE before tearing the window down, which is the
     * widget's only chance to release its timer. */
    tess_emwin_probe_send(window, WM_DELETE, NULL);
    w->in_use = false;
    record.windows_deleted++;
}

int WM_SetUserData(WM_HWIN window, const void *source, int bytes)
{
    probe_window *w = window_of(window);
    if (w == NULL || source == NULL || bytes < 0 || bytes > w->extra_bytes)
    {
        return 0;
    }
    memcpy(w->extra, source, (size_t) bytes);
    return bytes;
}

int WM_GetUserData(WM_HWIN window, void *destination, int bytes)
{
    probe_window *w = window_of(window);
    if (w == NULL || destination == NULL || bytes < 0 || bytes > w->extra_bytes)
    {
        return 0;
    }
    memcpy(destination, w->extra, (size_t) bytes);
    return bytes;
}

void WM_SetId(WM_HWIN window, int id)
{
    probe_window *w = window_of(window);
    if (w != NULL)
    {
        w->id = id;
    }
}

void WM_InvalidateWindow(WM_HWIN window)
{
    (void) window;
    record.invalidations++;
}

void WM_DefaultProc(WM_MESSAGE *message)
{
    (void) message;
    record.default_proc_calls++;
}

int WM_GetWindowOrgX(WM_HWIN window)
{
    const probe_window *w = window_of(window);
    return (w != NULL) ? w->x0 : 0;
}

int WM_GetWindowOrgY(WM_HWIN window)
{
    const probe_window *w = window_of(window);
    return (w != NULL) ? w->y0 : 0;
}

int WM_GetWindowSizeX(WM_HWIN window)
{
    const probe_window *w = window_of(window);
    return (w != NULL) ? w->width : 0;
}

int WM_GetWindowSizeY(WM_HWIN window)
{
    const probe_window *w = window_of(window);
    return (w != NULL) ? w->height : 0;
}

WM_HTIMER WM_CreateTimer(WM_HWIN window, int user_id, int period_ms, int mode)
{
    (void) window;
    (void) user_id;
    (void) period_ms;
    (void) mode;

    record.timers_created++;
    return next_timer++;
}

void WM_RestartTimer(WM_HTIMER timer, int period_ms)
{
    (void) timer;
    (void) period_ms;
    record.timers_restarted++;
}

void WM_DeleteTimer(WM_HTIMER timer)
{
    (void) timer;
    record.timers_deleted++;
}

/* ---------------------------------------------------------------- drawing - */

void GUI_SetColor(GUI_COLOR colour) { (void) colour; }
void GUI_SetBkColor(GUI_COLOR colour) { (void) colour; }
void GUI_SetFont(const GUI_FONT *font) { (void) font; }
void GUI_Clear(void) {}
void GUI_FillRect(int x0, int y0, int x1, int y1) { (void) x0; (void) y0; (void) x1; (void) y1; }
void GUI_DrawRect(int x0, int y0, int x1, int y1) { (void) x0; (void) y0; (void) x1; (void) y1; }

void GUI_ClearRect(int x0, int y0, int x1, int y1)
{
    (void) x0; (void) y0; (void) x1; (void) y1;
    record.rects_cleared++;
}

void GUI_FillCircle(int x0, int y0, int radius)
{
    (void) x0; (void) y0; (void) radius;
    record.circles_drawn++;
}

void GUI_DispStringHCenterAt(const char *text, int x, int y)
{
    (void) text; (void) x; (void) y;
    record.strings_drawn++;
}

void GUI_DrawBitmap(const GUI_BITMAP *bitmap, int x, int y)
{
    (void) bitmap;
    record.bitmaps_drawn++;
    record.last_bitmap_x = x;
    record.last_bitmap_y = y;
}

void GUI_CreateBitmapFromStreamA565(GUI_BITMAP *bitmap, GUI_LOGPALETTE *palette,
                                    const void *stream)
{
    record.streams_parsed++;

    if (bitmap == NULL)
    {
        return;
    }

    memset(bitmap, 0, sizeof(*bitmap));
    bitmap->XSize = 256;
    bitmap->YSize = 256;
    bitmap->BytesPerLine = 512;
    bitmap->BitsPerPixel = 16;
    bitmap->pData = (const uint8_t *) stream;
    bitmap->pMethods = GUI_DRAW_BMPA565;

    if (palette != NULL)
    {
        memset(palette, 0, sizeof(*palette));
    }
}

/* ---------------------------------------------------------- memory devices */

GUI_MEMDEV_Handle GUI_MEMDEV_CreateFixed(int x0, int y0, int width, int height,
                                         int flags, const void *api_list,
                                         const void *colour_conversion)
{
    (void) x0; (void) y0; (void) width; (void) height;
    (void) flags; (void) api_list; (void) colour_conversion;

    for (int i = 0; i < MAX_MEMDEVS; i++)
    {
        if (memdevs[i])
        {
            continue;
        }
        memdevs[i] = true;
        live_memdevs++;
        record.memdevs_created++;
        if (live_memdevs > record.peak_memdevs)
        {
            record.peak_memdevs = live_memdevs;
        }
        return i + 1;
    }
    return 0;  /* exhausted -- the case a caller must handle */
}

void GUI_MEMDEV_Select(GUI_MEMDEV_Handle device) { (void) device; }

void GUI_MEMDEV_Delete(GUI_MEMDEV_Handle device)
{
    if (device >= 1 && device <= MAX_MEMDEVS && memdevs[device - 1])
    {
        memdevs[device - 1] = false;
        live_memdevs--;
        record.memdevs_deleted++;
    }
}

void GUI_MEMDEV_WriteAt(GUI_MEMDEV_Handle device, int x, int y)
{
    (void) device; (void) x; (void) y;
    record.memdev_writes++;
}

void GUI_MEMDEV_RotateHQ(GUI_MEMDEV_Handle source, GUI_MEMDEV_Handle destination,
                         int dx, int dy, int32_t angle_millideg, int32_t magnify)
{
    (void) source; (void) destination; (void) dx; (void) dy;
    (void) angle_millideg; (void) magnify;
    record.rotations++;
}
