# Tessera

A slippy-map widget for embedded displays. Tiles from local storage, panning,
zooming, and markers with headings and off-screen direction arrows — on a
microcontroller with no GPU and no network.

Portable C11 with no dependencies, plus a **SEGGER emWin binding**, a FatFs
tile source and a CMSIS-RTOS loader.

```
9,820 bytes of code on a Cortex-M4F at -Os        0 bytes of static RAM
30,505 assertions, in a fifth of a second         gcc · clang · ASan · UBSan · TSan
```

![A 480x272 map view: synthetic checkerboard tiles with visible seams, a red
focus marker at the centre of the viewport and a yellow waypoint to the
north-east](docs/images/demo.png)

<sub>Rendered by `tessera_demo` — a 480×272 viewport, synthetic tiles, a focus
marker at the centre and a waypoint to the north-east. Tile seams are drawn
deliberately, so a tile placed one row out is obvious.</sub>

## Why this exists

Map widgets on small displays tend to be written as one file: the Web Mercator
arithmetic, a request queue, a tile cache, the filesystem reads, a loader
thread and the drawing, all together. It works, and then nothing about it can
be checked without the display, the storage and the RTOS — so the parts that
*are* checkable, which is most of them, never get checked.

Tessera splits that seam. The engine knows nothing about how tiles are stored
or drawn; it asks a vtable. So the tile addressing, the cache eviction, the
queue, the viewport transforms and the marker placement all run on a
workstation, and the display-specific code shrinks to something you can read in
one sitting.

## Layout

| | |
| --- | --- |
| `include/tessera/`, `src/` | The engine. C11 and libm, nothing else. |
| `port/emwin/` | The emWin binding: a `WM` callback, a painter, drag-to-pan. |
| `port/fatfs/` | Tile source over a FatFs volume. |
| `port/cmsis-rtos/` | The mutex and the loader loop. |
| `port/framebuffer/` | A software renderer and a synthetic tile source — no third-party dependencies, used by the tests and the demo. |
| `port/*/interop/` | Independent declarations of the emWin, FatFs and CMSIS APIs, so those ports are compiled and exercised in CI. [Read this.](port/emwin/interop/README.md) |
| `docs/` | [Architecture](docs/ARCHITECTURE.md) · [Porting](docs/PORTING.md) · [emWin](docs/EMWIN.md) · [Design notes](docs/DESIGN-NOTES.md) |

## Building

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure     # 30,505 checks
```

Everything that has to pass — gcc, clang, sanitizers, ThreadSanitizer and the
Cortex-M4F cross-build — is one script:

```sh
tools/check-all.sh
```

Render frames you can look at:

```sh
mkdir -p /tmp/frames && ./build/tessera_demo /tmp/frames
# six PPMs: initial, pan, zoom about an anchor, edge arrow, fit, missing tiles
```

## Using it

The engine owns no memory. Cache slots, tile buffers and rotation scratch all
come from the caller, so where they live is your linker script's business —
which matters when the tile buffers are the largest allocation in the firmware.

```c
#include "tessera/map.h"

static uint8_t buffers[25][TESS_TILE_BYTES_16BPP];
static tess_slot slots[25];
for (int i = 0; i < 25; i++) slots[i].image = buffers[i];

tess_map_config cfg = {0};
cfg.centre = home;  cfg.zoom = 15;
cfg.width  = 480;   cfg.height = 272;
cfg.slots  = slots; cfg.slot_count = 25;
cfg.marker_edge_inset = 16;
cfg.source = &tile_source;   /* FatFs, or your own */
cfg.lock   = &rtos_lock;     /* NULL if single-threaded */

tess_map map;
tess_map_init(&map, &cfg);
```

From the UI thread:

```c
tess_map_marker_set(&map, TESS_MARKER_FOCUS, position, "Here");
tess_map_marker_set_heading(&map, TESS_MARKER_FOCUS, bearing);
tess_map_pan(&map, dx, dy);
tess_map_zoom_at(&map, +1, tap_point);       /* keeps the tap under the finger */
tess_map_fit_markers(&map, 32);
tess_map_draw(&map, &painter);
```

From the loader thread:

```c
for (;;) {
    if (tess_map_service(&map) == TESS_ERR_EMPTY) osDelay(20);
}
```

With emWin, `TESSERA_CreateEx` wires all of that to a window and you reach the
engine through `TESSERA_GetMap(handle)`. See [`docs/EMWIN.md`](docs/EMWIN.md).

## Tiles

Standard slippy-map addressing: 256×256 tiles, 2^z per axis at zoom z, tile
(0,0) at the north-west, x east and y south. Zoom 0–22 by default; narrow it to
what your device ships with `-DTESSERA_MIN_ZOOM=10 -DTESSERA_MAX_ZOOM=16`.

Storage layout is a pattern, so anything a tile cutter already produces works:

```c
"{z}/{x}/{y}.bin"          /* the default */
"tiles/{z}/{x}/{y}.raw"
"map/t_{x}_{y}_{z}.bin"    /* flat, for filesystems without deep trees */
```

Tessera does not decode images. A cache slot holds whatever bytes the tile
source put there, and the painter interprets them — raw RGB565, an emWin bitmap
stream, or anything else. No decoder means no format lock-in and no attack
surface from a parser.

## What it handles that is easy to get wrong

Each of these has tests; several were found *by* the tests.

**Latitude near the poles.** Web Mercator is undefined at ±90°, where `tan()`
runs away. Latitude is clamped to ±85.0511°, and every tile address is
range-checked before it can become a path — a GNSS receiver reporting garbage
during cold start is the ordinary way an out-of-range row appears.

**The antimeridian.** Tile columns wrap; a marker just west of 180° is a few
pixels from a view just east of it, not most of a world. Both the tile blit
origin and the pixel offset take the short way round.

**Tile boundaries.** A boundary is half-open, so a position exactly on a tile's
north edge belongs to *that* tile — but the round trip through the projection
lands a few ulp short and `floor()` disagrees. Coordinates within tolerance are
snapped, and the tolerance scales with zoom because the round-trip error does.

**Off-screen markers.** Placed by ray–rectangle intersection: one scale factor,
no quadrant special cases, and the corners and diagonals fall out of it. Tested
by sweeping all 360 degrees.

**Publishing a half-decoded tile.** A cache slot becomes drawable at exactly one
point, after its image is complete. The reader reserves a slot *before* opening
anything, so the "no room" path — which a full cache makes routine — cannot leak
a file handle.

**Concurrency.** The engine brackets its own shared state and never holds a lock
across a read. The loader loop therefore holds no lock at all, and CI runs it
under ThreadSanitizer against a thread that pans continuously.

## Status

Working and covered, but **not yet run on hardware.** The emWin, FatFs and
CMSIS ports are compiled and exercised against independent declarations of
those APIs rather than the libraries themselves, so their own logic is
established and their integration is not. If you are the first to put this on a
display, [`docs/EMWIN.md`](docs/EMWIN.md) lists what to check, and a report
either way is welcome.

## Licence

MIT — see [LICENSE](LICENSE). No emWin, FatFs or CMSIS source is included;
emWin is commercially licensed by SEGGER and this repository is not a
substitute for that licence.
