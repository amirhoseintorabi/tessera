# The emWin binding

`port/emwin/tessera_emwin.c` is a custom SEGGER emWin window that draws a
Tessera map: a `WM` callback, a painter built from `GUI_*` calls, drag-to-pan
on `WM_TOUCH`, and a timer that repaints as tiles arrive.

emWin is commercially licensed by SEGGER. **This repository contains no emWin
source, headers or binaries**, and is not a substitute for a licence.

## Using it

```c
#include "tessera_emwin.h"

static uint32_t rotation_scratch[32 * 32];

TESSERA_CONFIG cfg = {0};
cfg.map             = &map;          /* an initialised tess_map */
cfg.vehicle         = &bm_arrow;     /* drawn rotated to the heading */
cfg.pin             = &bm_pin;
cfg.arrow           = &bm_edge;      /* drawn rotated to the bearing */
cfg.rotation_buffer = rotation_scratch;
cfg.rotation_size   = 32;
cfg.refresh_ms      = 100;
cfg.touch_pan       = true;
cfg.placeholder_bk  = GUI_DARKGRAY;
cfg.placeholder_fg  = GUI_LIGHTGRAY;

WM_HWIN h = TESSERA_CreateEx(&cfg, parent, 0, 0, 480, 272, GUI_ID_USER + 1);
```

Then reach the engine through the handle:

```c
tess_map *m = TESSERA_GetMap(h);
tess_map_marker_set(m, TESS_MARKER_FOCUS, position, "Here");
tess_map_marker_set_heading(m, TESS_MARKER_FOCUS, bearing);
WM_InvalidateWindow(h);
```

Every bitmap is optional. With none configured, markers draw as filled circles
— enough to bring a board up before the artwork exists.

## Building

Point the include path at your licensed emWin:

```sh
cmake -B build -DTESSERA_EMWIN_DIR=/path/to/emWin/Include
cmake --build build --target tessera_ports
```

The binding then includes `"GUI.h"` and `"WM.h"` by their own names, so nothing
in the port changes. Without `TESSERA_EMWIN_DIR` the build defines
`TESSERA_EMWIN_INTEROP` and falls back to the declarations in
`port/emwin/interop/` — see [that directory's README](../port/emwin/interop/README.md).
No file in this repository is named `GUI.h` or `WM.h`.

## Design decisions

Two departures from how an emWin composite widget is usually written, both
deliberate.

**No child widgets.** The natural approach is an `IMAGE` per tile position,
panned by moving them. That is one window object per tile plus the
invalidation traffic to match, and it gives you two sources of truth for where
things are — the window positions and the map's view — which drift apart. The
whole map is drawn in `WM_PAINT` instead. The blits happen either way.

**One rotation buffer, not one per marker.** Painting is synchronous, so only
one marker is being rotated at a time. A persistent rotated bitmap per marker
would be ten buffers where one will do, and each would need invalidating
whenever its heading changed. A test asserts that no more than two memory
devices are ever alive at once, whatever the marker count.

Two smaller ones:

**The timer invalidates only when something changed.** `WM_TIMER` always
restarts the timer, but only calls `WM_InvalidateWindow` while
`tess_map_pending()` is non-zero. Repainting on every tick regardless is a
screenful of blits per tick that nobody asked for, competing with the loader
for the memory bus.

**State lives in the window's extra bytes, the map does not.** emWin's user
data is a copy in and a copy out. The state struct is about sixty bytes — a
config, a timer handle, three integers of drag state — and the map is
referenced by pointer. Putting the map itself there would make every field
access a round trip over the whole structure.

## Messages handled

| | |
| --- | --- |
| `WM_PAINT` | draws the map through a `tess_painter` built from `GUI_*` calls |
| `WM_TOUCH` | drag-to-pan when `touch_pan` is set; a lift or a NULL payload ends the drag |
| `WM_TIMER` | restarts itself; invalidates only if tiles are still arriving |
| `WM_DELETE` | releases the timer |
| everything else | `WM_DefaultProc` |

`TESSERA_Callback` is exported, so an application that needs its own callback
can chain to it rather than reimplement it.

## What is verified, and what is not

CI compiles this file under the project's full warning set and drives it
against a recorder that implements the emWin API surface it calls. That
establishes the binding's own logic:

- one bitmap drawn per loaded tile, one placeholder per missing one
- drawing offset correctly by the window origin
- memory devices created and destroyed in pairs, never more than two alive
- drag panning that follows the finger and stops when it lifts
- the timer restarting but not invalidating when nothing changed
- the timer released on `WM_DELETE`

It establishes **nothing about emWin itself**. A recorder cannot tell you
whether emWin's clipping, its memory-device rotation, its bitmap-stream parsing,
the exact semantics of `WM_CF_MEMDEV` or the message ordering around `WM_TOUCH`
behave as this code assumes.

### First time on hardware

In roughly this order, because each depends on the last:

1. **One tile, drawn at all.** Configure a 1×1 grid and a single marker. If the
   tile is blank, the tile source is the suspect, not the binding —
   `tess_map_get_stats()` distinguishes `tiles_loaded` from `tiles_missing` and
   `tiles_failed`.
2. **Placement.** Put the widget somewhere other than (0, 0). Tiles drawn at
   the right place only when the widget is at the origin means the window
   origin is being added zero times or twice.
3. **Clipping.** Grid tiles extend past the viewport by design. If edge tiles
   are missing or the widget draws outside itself, `GUI_DrawBitmap` is not
   clipping the way this assumes.
4. **The bitmap stream.** `paint_tile` calls
   `GUI_CreateBitmapFromStreamA565`, which expects the tile bytes to be an
   emWin bitmap stream. If your tiles are raw pixels, swap that call for a
   `GUI_BITMAP` filled in directly.
5. **Rotation.** Set a heading and watch the marker turn about its own centre.
   If it orbits instead of spinning, the artwork is not centred in the
   rotation square.
6. **Touch.** Drag. The map should follow the finger. If it moves the wrong
   way, the sign convention in `on_touch` disagrees with your PID driver.
7. **Under load.** Pan continuously while the loader runs. Watch
   `tess_map_get_stats()` for `cache_full` and `queue_overflows` — both should
   stay at zero with a correctly sized cache.

If you get through that list, please open an issue saying so. "It works" is
information this repository does not currently have.
