/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_EMWIN_API_H
#define TESSERA_EMWIN_API_H

/*
 * Interoperability declarations for the subset of the SEGGER emWin API that
 * ../tessera_emwin.c calls.
 *
 * NOT SEGGER's headers, not derived from them, and containing no emWin code.
 * These are hand-written from the published API so that the binding can be
 * compiled under the project's warning settings and driven by a test.
 *
 * The file is named for this project rather than for emWin, deliberately: a
 * repository should not ship files whose names collide with a commercial
 * vendor's, however different the contents. When you build against a licensed
 * emWin, nothing here is compiled at all -- see the include guard in
 * ../tessera_emwin.h.
 *
 * emWin is commercially licensed by SEGGER, and nothing here substitutes for a
 * licence. What these declarations cannot establish is emWin's own behaviour;
 * see README.md.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- drawing API */

#include <stdint.h>


typedef uint32_t GUI_COLOR;
typedef int GUI_MEMDEV_Handle;

#define GUI_MAKE_COLOR(c) ((GUI_COLOR)(c))

#define GUI_BLACK       ((GUI_COLOR) 0x000000u)
#define GUI_WHITE       ((GUI_COLOR) 0xFFFFFFu)
#define GUI_RED         ((GUI_COLOR) 0x0000FFu)
#define GUI_YELLOW      ((GUI_COLOR) 0x00FFFFu)
#define GUI_CYAN        ((GUI_COLOR) 0xFFFF00u)
#define GUI_GRAY        ((GUI_COLOR) 0x808080u)
#define GUI_TRANSPARENT ((GUI_COLOR) 0xFF000000u)

#define GUI_MEMDEV_NOTRANS 0
#define GUI_MEMDEV_HASTRANS 1

#define GUI_ID_USER 0x800

typedef struct
{
    int16_t XSize;
    int16_t YSize;
    int16_t BytesPerLine;
    int16_t BitsPerPixel;
    const uint8_t *pData;
    const void *pPal;
    const void *pMethods;
} GUI_BITMAP;

typedef struct
{
    int NumEntries;
    int HasTrans;
    const GUI_COLOR *pPalEntries;
} GUI_LOGPALETTE;

typedef struct
{
    int XSize;
    int YSize;
    int Baseline;
} GUI_FONT;

typedef struct
{
    int x0, y0, x1, y1;
} GUI_RECT;

/* Colour-conversion and memory-device API lists are opaque handles as far as
 * the widget is concerned. */
extern const void *GUICC_8888;
extern const void *GUI_MEMDEV_APILIST_32;
extern const void *GUI_DRAW_BMPA565;

void GUI_SetColor(GUI_COLOR colour);
void GUI_SetBkColor(GUI_COLOR colour);
void GUI_SetFont(const GUI_FONT *font);
void GUI_Clear(void);
void GUI_ClearRect(int x0, int y0, int x1, int y1);
void GUI_FillRect(int x0, int y0, int x1, int y1);
void GUI_DrawRect(int x0, int y0, int x1, int y1);
void GUI_FillCircle(int x0, int y0, int radius);
void GUI_DispStringHCenterAt(const char *text, int x, int y);
void GUI_DrawBitmap(const GUI_BITMAP *bitmap, int x, int y);

void GUI_CreateBitmapFromStreamA565(GUI_BITMAP *bitmap, GUI_LOGPALETTE *palette,
                                    const void *stream);

GUI_MEMDEV_Handle GUI_MEMDEV_CreateFixed(int x0, int y0, int width, int height,
                                         int flags, const void *api_list,
                                         const void *colour_conversion);
void GUI_MEMDEV_Select(GUI_MEMDEV_Handle device);
void GUI_MEMDEV_Delete(GUI_MEMDEV_Handle device);
void GUI_MEMDEV_WriteAt(GUI_MEMDEV_Handle device, int x, int y);
void GUI_MEMDEV_RotateHQ(GUI_MEMDEV_Handle source, GUI_MEMDEV_Handle destination,
                         int dx, int dy, int32_t angle_millideg, int32_t magnify);

/* ------------------------------------------------------------ window manager */

#include <stdint.h>



typedef int WM_HWIN;
typedef int WM_HTIMER;
typedef int WM_HMEM;

/* The subset of emWin's message identifiers the widget handles. The real
 * values are internal to emWin; only their distinctness matters here. */
#define WM_PAINT       0x0F
#define WM_TOUCH       0x10
#define WM_TIMER       0x11
#define WM_DELETE      0x12
#define WM_GET_ID      0x13
#define WM_POST_PAINT  0x14

#define WM_CF_SHOW      (1u << 0)
#define WM_CF_MEMDEV    (1u << 1)
#define WM_CF_HASTRANS  (1u << 2)
#define WM_CF_STAYONTOP (1u << 3)

typedef struct
{
    int x;
    int y;
    int Pressed;
    int Layer;
} GUI_PID_STATE;

typedef struct WM_MESSAGE
{
    int MsgId;
    WM_HWIN hWin;
    WM_HWIN hWinSrc;
    union
    {
        const void *p;
        int v;
        GUI_COLOR Color;
    } Data;
} WM_MESSAGE;

typedef void WM_CALLBACK(WM_MESSAGE *message);

WM_HWIN WM_CreateWindowAsChild(int x0, int y0, int width, int height, WM_HWIN parent,
                               uint32_t style, WM_CALLBACK *callback, int extra_bytes);
void WM_DeleteWindow(WM_HWIN window);

int  WM_SetUserData(WM_HWIN window, const void *source, int bytes);
int  WM_GetUserData(WM_HWIN window, void *destination, int bytes);

void WM_SetId(WM_HWIN window, int id);
void WM_InvalidateWindow(WM_HWIN window);
void WM_DefaultProc(WM_MESSAGE *message);

int WM_GetWindowOrgX(WM_HWIN window);
int WM_GetWindowOrgY(WM_HWIN window);
int WM_GetWindowSizeX(WM_HWIN window);
int WM_GetWindowSizeY(WM_HWIN window);

WM_HTIMER WM_CreateTimer(WM_HWIN window, int user_id, int period_ms, int mode);
void WM_RestartTimer(WM_HTIMER timer, int period_ms);
void WM_DeleteTimer(WM_HTIMER timer);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_EMWIN_API_H */
