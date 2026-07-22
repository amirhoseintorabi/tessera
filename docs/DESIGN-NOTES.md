# Design notes

Decisions that are not obvious from the code, and the reasoning behind them.
Mostly places where the tempting version is subtly wrong.

## Coordinates

**One source of truth.** Every position — the tile that gets drawn, the pixel
it is blitted at, the pixel a marker lands on — is derived from a single
quantity: the view centre's continuous tile coordinate. Deriving them
separately means reconciling them with per-axis correction terms, and the
corrections are where sign errors hide. The cost is one extra transform per
frame.

**`double`, not `float`.** At zoom 16 one pixel is about 2.4e-6 degrees of
longitude, and `float` carries roughly 7 significant decimal digits — so around
50°N the smallest representable step is coarser than a pixel, and worse above
zoom 16. Positions are passed around a handful of times per frame, not per
pixel, so the wider type does not show up in a profile.

**Shifting, not `pow`.** `pow(2, 16)` is not guaranteed to return exactly
65536.0 on every libm, and one ulp low truncates to 65535 when it lands in an
`int` — which puts the whole map one tile out.

## The boundary snap

A tile boundary is a half-open interval: a position exactly on tile *N*'s
northern edge belongs to *N*, not *N−1*. But round-tripping a boundary through
the projection does not land exactly on the integer — it lands a few ulp below
— and `floor()` then returns *N−1*, with a pixel offset of 255 instead of 0.

On screen that is a one-tile jump whenever the view centre crosses a boundary,
and a marker that flicks to the opposite edge as it crosses one.

Coordinates within tolerance of an integer are snapped to it. **The tolerance
scales with the coordinate**, because the round-trip error does: a double
carries ~16 significant digits, and the coordinate runs to 2^zoom — 65,536 at
zoom 16 but 4,194,304 at zoom 22. A fixed absolute tolerance that is generous
at one end of that range is below the noise floor at the other. 1e-12 relative
with a 1e-9 absolute floor works out to a thousandth of a pixel at zoom 22.

This one is easy to miss because it is invisible at the zoom levels you
happen to test at.

## The antimeridian

Longitude wraps; tile columns wrap with it. Two points either side of 180° are
a few pixels apart on the ground and almost a whole world apart in raw tile
coordinates.

Both `tess_pixel_delta` and `tess_view_tile_origin` take the short way round —
if the difference exceeds half the world, a world is added or subtracted. Without
it, a marker just west of the line is placed off the far edge of a view centred
just east of it, and the grid column immediately west of the centre is drawn a
whole world away to the east.

`y` is deliberately **not** wrapped. There is nothing above the north edge to
wrap onto, and wrapping would put an antarctic tile above an arctic one — worse
than a blank, because it looks like data. Those addresses come back as ones
`tess_tile_is_valid` rejects.

## The zoom floor

Below a certain zoom the whole world is narrower than the viewport, the same
place appears more than once on screen, and "where is this position" stops
having one answer. Everything that maps between screen and ground assumes it
has one.

`tess_view` clamps to `tess_min_zoom_for_width(width)` rather than producing a
second copy of the world that nothing else knows about. At 256 pixels per tile
that is zoom 1 for a 480-pixel viewport and zoom 3 for a 1024-pixel one, so on
a real display it costs nothing.

Supporting horizontal world-repeat would be the alternative. It is a real
feature and it is not in scope here.

## Half-open rectangles

`tess_rect` is `x0 <= x < x1`. Width is `x1-x0` rather than `x1-x0+1`, adjacent
rectangles tile the plane without overlapping, and a marker exactly on the left
edge reads as inside.

That last one matters: with an inclusive rectangle and a strict `>` containment
test, a marker on the edge reads as outside, and a widget that draws off-screen
markers differently then draws it twice — once in place and once as an edge
arrow.

`tess_rect_inset` never returns an inverted rectangle. Past the point where the
edges would cross it collapses to the centre, because an inverted rectangle
answers "outside" for every point, which reads like a marker that has left the
screen rather than like a bad argument.

## Off-screen markers

Ray–rectangle intersection, not quadrants.

The ray leaves through the vertical edges after scaling by `hx/|dx|` and the
horizontal ones after `hy/|dy|`. The smaller scale is the edge it actually
crosses. That is the entire case analysis: corners and diagonals fall out of
it, and an axis-aligned ray is handled by a division guard rather than a
special case.

Splitting the viewport into four triangular regions by its diagonals is the
approach that suggests itself, and it needs four formulas that agree at three
boundaries each, plus special cases for a marker exactly on a diagonal and one
exactly at the centre. Any region-based implementation is wrong somewhere, and
where it is wrong is usually a boundary — which is why the tests sweep all 360
degrees rather than sampling a few directions.

**Bearings are compass, not mathematical.** Screen `y` grows downwards while
north is up, so it is `atan2(dx, -dy)`, not `atan2(dy, dx)`. That is exposed as
`tess_bearing_from_offset` so that a painter rotating a bitmap uses the same
convention — two places deriving it independently is how the sign gets flipped.

## The cache

**Reserve before opening.** The reader has nowhere to put bytes until
`acquire` succeeds, so the "no room" exit — which a full cache makes routine —
is taken before anything has been opened. It cannot leak a handle, because
there is nothing open to leak.

**Commit is the only way in.** A slot becomes visible to the drawing side at
one point, after its image is complete. Setting a "ready" flag and then filling
the buffer leaves a window exactly as wide as the decode, on the other thread.

**Eviction is a timestamp comparison.** "Still wanted" is `used_at` against the
current frame, not a flag someone clears. A slot that stops being wanted
becomes reusable by doing nothing at all — there is no recycling pass to
forget, or to write as a comparison instead of an assignment.

**LOADING slots are never evicted**, so `acquire` can return `TESS_ERR_FULL`.
That is the honest answer; evicting a reservation would have two readers
writing one buffer, and whichever finished second would be committed under the
first one's address.

**Sizing.** At least the grid, or the cache evicts a tile it is about to want
again and the map thrashes without settling — caught at `init` rather than left
as a baffling runtime symptom. Twice the grid makes zooming back free. A test
demonstrates both regimes, including the one where a grid-sized cache does not
retain the level you came from; that is arithmetic, not a defect.

## Concurrency

**Use the RTOS's mutex.** A flag with a wait loop is not a lock: the test and
the set are separate statements with a scheduling point between them, so two
threads can both observe "free"; and unless the flag is atomic the compiler may
hoist the load out of the loop and spin on a stale value. `volatile` addresses
neither — it constrains the compiler's caching of a value and says nothing
about ordering between threads.

ThreadSanitizer made that point concretely during development: the loader's
stop flag was `volatile bool`, which is the reflex, and TSan called it a data
race. Correctly. On a Cortex-M4 a byte load is atomic in practice and it would
very likely have worked — but "very likely works" is the property that makes a
threading bug survive testing and appear in the field. It is guarded by the
mutex now.

**Never hold a lock across a read.** The read is tens of milliseconds; the
critical sections are pointer comparisons. `tess_map_service` brackets its own
state in three short phases with the medium access outside all of them, which
is why the loader loop can hold no lock at all — and a loop with no lock in it
cannot forget to release one on a failure path.

**A non-recursive mutex is sufficient**, and a test asserts the lock is never
taken twice deep, so a port cannot deadlock against itself.

## Ownership

The library allocates nothing and has no static state — the ARM build reports
0 bytes of `.data` and `.bss`. Cache slots, tile buffers and rotation scratch
all come from the caller.

On a small part that is not fastidiousness: tile buffers are usually the
largest single allocation in the firmware, and often have to be placed in a
particular memory region by the linker script. A library that allocated them
would have to be told how, and would get it wrong for somebody.

It also means the pixel format is not the library's business. A cache slot
holds whatever bytes the tile source put there and the painter interprets them,
so there is no decoder — no format lock-in, and no parser to attack.

## Errors

**Fail closed, and say which.** `tess_tile_path` empties the buffer before
returning an error, so a caller that ignores the status gets an empty string
rather than stack contents. Distinct codes for "the medium has no such tile"
and "the medium failed" matter: the first is normal near the edge of a coverage
area and the engine draws a placeholder; the second is worth surfacing.

**Nothing reports success without writing its output.** A search that returns
true on a miss hands the caller an uninitialised index, and the caller will use
it as an array subscript. Two tests exist purely to pin this down.

**Validate at the boundary.** `tess_tile_is_valid` is checked before an address
becomes a path, is queued, or is given a cache slot. Impossible addresses are
produced routinely — an unclamped latitude, an unwrapped column, a GNSS
receiver during cold start — and the cheapest place to stop them is before
they reach anything that formats or opens.
