# Architecture

Six pieces of logic, three vtables, and nothing else.

```
      your application
            │
   ┌────────┴─────────┐
   │     tess_map     │   the engine: owns the view, the grid, the cache,
   │                  │   the queue and the markers
   └───┬───────┬──────┘
       │       │
  ┌────┴───┐ ┌─┴──────────────────────────────────┐
  │ logic  │ │ ports (vtables in tessera/port.h)  │
  │        │ │                                    │
  │ view   │ │ tess_tile_source   where tiles     │
  │ grid   │ │                    come from       │
  │ cache  │ │ tess_painter       how they are    │
  │ queue  │ │                    drawn           │
  │ marker │ │ tess_lock          mutual          │
  │ project│ │                    exclusion       │
  └────────┘ └────────────────────────────────────┘
```

Everything on the left is pure C11 and libm: no display, no filesystem, no
scheduler. Everything platform-specific enters through the right, and nowhere
else.

## The layers

### `projection` — Web Mercator

Geographic position ↔ tile coordinate, in both directions and in continuous
form. Everything positional in the library is derived from
`tess_geo_to_tile_f`, so the tile that gets drawn, the pixel it is drawn at and
the pixel a marker lands on cannot disagree with one another.

The continuous inverse `tess_tile_f_to_geo` is what makes drag-to-pan,
tap-to-place and zoom-about-a-point possible; `tess_tile_north_west` is defined
in terms of it rather than being a second copy of the formula.

### `view` — the viewport

Centre, zoom, and widget size. Turns those into: which tile is at the centre,
where a given tile's north-west corner belongs on screen, where a position
lands, and what position is under a pixel.

Panning moves the centre in pixels. Zooming takes an anchor point and keeps
whatever was under it under it. Both clamp: latitude to the projectable range,
zoom to `[tess_min_zoom_for_width(width), TESSERA_MAX_ZOOM]`.

### `grid` — the tile window

An odd number of columns by an odd number of rows around the centre tile, so
there is always an unambiguous middle. Two jobs: which address is at grid
position (col, row), and in what order should they be fetched.

Fetch order is centre-out — rings of increasing Chebyshev distance, nearest
first within a ring. That is what the user experiences: tiles arrive over tens
of milliseconds each, so filling the middle first is the difference between a
pan that feels immediate and one that does not.

### `queue` — the request queue

A bounded ring of tile addresses. Deduplicating, so recomputing the wanted set
on every viewport change does not re-fetch what is already coming. No locking
and no I/O, so it is exhaustively testable.

### `cache` — the tile slots

A fixed set of slots, each holding one tile image in caller-owned memory. Three
states and three transitions:

```
   FREE ──acquire──▶ LOADING ──commit──▶ READY
     ▲                   │                 │
     └──────release──────┘                 │
     └──────────────evict (LRU)────────────┘
```

The separation is the point:

- **acquire before any I/O.** The reader has nowhere to put bytes until it
  succeeds, so the "no room" exit — routine when the cache is full — is taken
  before anything has been opened, and cannot leak a handle.
- **commit is the only way in.** A slot becomes visible to the drawing side at
  one point, after its image is complete. There is no window in which a
  partly-written image is findable.
- **eviction is a timestamp comparison**, not a flag anyone clears. A slot that
  stops being wanted becomes reusable by doing nothing.

LOADING slots are never evicted. When every slot is in flight, `acquire`
returns `TESS_ERR_FULL` — the honest answer; the alternative would be two
readers writing one buffer.

### `marker` — markers and edge arrows

Position, optional heading, label. The interesting half is the off-screen case:
a marker outside the view becomes an arrow on the viewport edge pointing at it.

That is a ray–rectangle intersection. Take the ray from the centre towards the
marker; it leaves through the vertical edges after scaling by `hx/|dx|` and
the horizontal ones after `hy/|dy|`; the smaller scale is the edge it actually
crosses. Corners and diagonals fall out of it, and there are no quadrants.

## The two threads

```
UI thread                          loader thread
─────────                          ─────────────
tess_map_pan / zoom / set_centre
  └─ tess_map_refresh              tess_map_service
       [lock] recompute wanted       [lock] peek queue, reserve slot [unlock]
              set, queue misses      ─────▶ source->load()   ← the slow part,
       [unlock]                              no lock held
                                     [lock] commit or release [unlock]
tess_map_draw
  [lock] look up slot [unlock]
  painter->draw_tile(...)
```

The lock is held only around pointer comparisons and state changes, never
across a read from the medium. A read takes tens of milliseconds; holding a
mutex across it would stall drawing for exactly that long.

Two consequences worth knowing:

- **The loader loop holds no lock itself.** `tess_map_service` does its own
  bracketing and releases on every path, including the failure paths. A loop
  that has no lock in it cannot forget to release one.
- **A non-recursive mutex is sufficient**, and a test asserts the lock is never
  taken twice deep, so a port cannot deadlock against itself.

`tess_map_draw` copies the image pointer out under the lock and blits outside
it. A `READY` slot only changes state through `acquire`, so the worst case is a
single frame drawing a tile that is being replaced.

## Memory

The library allocates nothing, ever. It has no static state either — the ARM
build reports **0 bytes** of `.data` and `.bss`.

| Thing | Owner | Typical size |
| --- | --- | --- |
| Cache slots | caller | `sizeof(tess_slot)` × N |
| Tile images | caller | 128 KB each at 16 bpp |
| `tess_map` | caller | ~1 KB, mostly markers and the grid order |
| Marker rotation scratch | caller (emWin port) | 4 KB at 32×32 |

Sizing the cache: at least the grid size, or the map thrashes. Twice the grid
if the memory is there — a new zoom level's tiles land in the spare half, so
zooming back costs nothing.

## Where to look

| Question | File |
| --- | --- |
| How is a position turned into a tile? | `src/projection.c` |
| Where does tile *N* get blitted? | `src/view.c` |
| What order do tiles load in? | `src/grid.c` |
| What gets evicted? | `src/cache.c` |
| Where does an off-screen arrow go? | `src/marker.c` |
| How do the threads interact? | `src/map.c`, `tess_map_service` |
| How do I add a display? | [`PORTING.md`](PORTING.md) |
