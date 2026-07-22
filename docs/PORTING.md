# Porting

Three vtables stand between the engine and your hardware. You need one of them
to see anything; the other two have sensible do-nothing defaults.

| | Required? | What it does |
| --- | --- | --- |
| `tess_painter` | yes | draws tiles and markers |
| `tess_tile_source` | no — without it the map draws placeholders | fetches tile bytes |
| `tess_lock` | no — NULL means single-threaded | mutual exclusion |

Working implementations of all three are in `port/`. The shortest path is to
copy the closest one and change the leaves.

## A painter

```c
static void my_draw_tile(void *ctx, const void *image, tess_tile tile,
                         tess_point origin)
{
    /* `image` is whatever your tile source put in the slot. `origin` is where
     * the tile's north-west corner goes, in widget-relative pixels; it may be
     * negative or past the edge, so clip. */
    blit_rgb565(ctx, image, origin.x, origin.y, 256, 256);
}

static void my_draw_placeholder(void *ctx, tess_tile tile, tess_point origin)
{
    fill_rect(ctx, origin.x, origin.y, 256, 256, GREY);
}

static void my_draw_marker(void *ctx, const tess_marker *marker,
                           tess_marker_placement placement, uint8_t index)
{
    /* placement.point is the marker's *anchor*, not its top-left corner --
     * subtract half your bitmap. */
    if (!placement.on_screen) {
        blit_rotated(ctx, arrow, placement.point, placement.bearing_deg);
    } else if (index == TESS_MARKER_FOCUS && marker->has_heading) {
        blit_rotated(ctx, glyph, placement.point, marker->heading_deg);
    } else {
        blit(ctx, pin, placement.point);
    }
}

const tess_painter painter = {
    .ctx              = &my_display,
    .draw_tile        = my_draw_tile,
    .draw_placeholder = my_draw_placeholder,
    .draw_marker      = my_draw_marker,
};
```

`begin` and `end` are optional hooks around the frame.

Three things to get right, because each is silently wrong rather than
obviously wrong:

- **Clip, do not assume.** Grid tiles extend past the viewport by design.
- **Add your window origin once.** The engine works in widget-relative pixels.
  If your graphics library draws in screen coordinates, add the offset in the
  painter — not at each call site, and not twice.
- **Anchor markers on their centre.** `placement.point` is where the marker
  *is*, not where its bitmap starts.

## A tile source

```c
static tess_status my_load(void *ctx, tess_tile tile, void *image)
{
    char path[TESS_TILE_PATH_MAX];
    if (tess_tile_path(path, sizeof path, "{z}/{x}/{y}.bin", tile) != TESS_OK)
        return TESS_ERR_ARG;

    size_t size;
    if (!stat_file(path, &size))    return TESS_ERR_NOT_FOUND;
    if (size > MY_SLOT_CAPACITY)    return TESS_ERR_RANGE;
    if (!read_file(path, image, size)) return TESS_ERR_IO;
    return TESS_OK;
}

const tess_tile_source source = { .ctx = &my_storage, .load = my_load };
```

The contract:

- Called **without the lock held**, on whichever thread calls
  `tess_map_service`. This is where the latency is allowed to be.
- `TESS_ERR_NOT_FOUND` means "the medium has no such tile" — normal near the
  edge of a coverage area, and the engine draws a placeholder rather than
  retrying.
- Anything other than `TESS_OK` releases the slot, so a failed read can never
  be drawn.

**Bound the read against the destination.** The engine cannot do it for you: it
does not know how large your buffers are. If the tiles come from removable
storage, their length is chosen by whoever last held the card. `port/fatfs`
takes the capacity as configuration for exactly this reason and refuses to
build a source without it.

## A lock

```c
static void my_acquire(void *ctx) { mutex_lock(ctx); }
static void my_release(void *ctx) { mutex_unlock(ctx); }

const tess_lock lock = { .ctx = &my_mutex, .acquire = my_acquire, .release = my_release };
```

Use your RTOS's mutex. A flag with a wait loop is not a lock — the test and the
set are separate statements with a scheduling point between them, and unless
the flag is atomic the compiler may hoist the load out of the loop and spin on
a stale value. `volatile` fixes neither.

The engine never takes the lock recursively, so a plain non-recursive mutex is
enough. A test asserts that.

## Wiring it up

```c
static uint8_t buffers[25][TESS_TILE_BYTES_16BPP];
static tess_slot slots[25];

for (int i = 0; i < 25; i++) slots[i].image = buffers[i];

tess_map_config cfg = {0};
cfg.centre = start;  cfg.zoom = 15;
cfg.width = 480;     cfg.height = 272;
cfg.slots = slots;   cfg.slot_count = 25;
cfg.marker_edge_inset = 16;      /* half your arrow bitmap */
cfg.source = &source;
cfg.lock   = &lock;              /* NULL if single-threaded */

if (tess_map_init(&map, &cfg) != TESS_OK) { /* see below */ }
```

`tess_map_init` fails rather than half-initialising. The cases:

| | |
| --- | --- |
| `slot_count` < grid size | the cache would evict a tile it is about to want again, and the map would thrash without ever settling |
| a slot with `image == NULL` | nowhere to decode into |
| an even `grid_cols` or `grid_rows` | no unambiguous centre tile |
| a non-positive width or height | nothing to draw on |

## Sizing

**Tile buffers.** `TESS_TILE_BYTES_16BPP` is 128 KB; a 5×5 grid is 3.1 MB. On a
part where that means external memory, place them with a section attribute:

```c
static uint8_t buffers[25][TESS_TILE_BYTES_16BPP] __attribute__((section(".sdram")));
```

**Grid.** Leave `grid_cols`/`grid_rows` at 0 and it is derived from the
viewport: enough to cover it plus one tile of margin each way, so a pan of up
to a tile does not expose an unloaded edge.

**Zoom range.** Default 0–22. Narrow it to what the device actually ships:

```
-DTESSERA_MIN_ZOOM=10 -DTESSERA_MAX_ZOOM=16
```

A view that cannot reach a zoom it has no tiles for is one fewer blank screen
to explain.

## Running it

Single-threaded — fine for a small map or a fast medium:

```c
while (tess_map_service(&map) == TESS_OK) { }
tess_map_draw(&map, &painter);
```

Two threads, which is what an SD card wants:

```c
/* loader */
for (;;) {
    if (tess_map_service(&map) == TESS_ERR_EMPTY) sleep_ms(20);
}
```

Sleep only when there is nothing to do. Sleeping after every tile caps the fill
rate regardless of how fast the medium is — at 20 ms that is 50 tiles a second
however quick the reads are.

## Checking your port

`tests/test_ports.c` is the template. Against a real filesystem it drives the
cases that matter and are awkward to reach by hand: a tile larger than the
slot, a truncated one, a missing one, and an address that cannot exist.

Worth running your own port under ASan with the destination buffer sized
*exactly* to one tile — a bounds check that is off by a little passes every
functional test and fails that one.
