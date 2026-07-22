/* SPDX-License-Identifier: MIT */
/*
 * Tests for the engine: the queue, the cache, the grid and the viewport
 * working together, driven through the same two port interfaces the emWin and
 * FatFs code implements.
 *
 * This is the level that a map widget usually cannot be tested at. Call the
 * filesystem, the graphics library and the scheduler from the same file as the
 * arithmetic, and checking any of it needs the display, the storage and the
 * RTOS. Everything below runs in a few milliseconds on a workstation against a
 * synthetic tile source that can be told to fail on demand.
 */

#include "framebuffer.h"

#include "check.h"
#include "fixture.h"

/*
 * Fifty slots for a twenty-five tile grid.
 *
 * Twice the grid is the sizing that makes a zoom change cheap: the new level's
 * tiles land in the free half, so the old level's are still there when the
 * user zooms straight back. A cache of exactly the grid size works, but every
 * zoom evicts the level you came from -- which is what
 * test_a_grid_sized_cache_cannot_retain_the_old_zoom demonstrates, and what
 * the sizing note in map.h is about.
 */
#define SLOTS 50


static uint16_t tile_pixels[SLOTS][TESSERA_TILE_SIZE * TESSERA_TILE_SIZE];
static tess_slot slots[SLOTS];
static uint8_t framebuffer[480 * 272 * 3];

/* A lock that records how it was used, so the tests can assert that the engine
 * takes it at all and never leaves it held. */
typedef struct
{
    int depth;
    int max_depth;
    uint32_t acquires;
    bool ever_negative;
} counting_lock;

static void counting_acquire(void *ctx)
{
    counting_lock *lock = (counting_lock *) ctx;
    lock->depth++;
    lock->acquires++;
    if (lock->depth > lock->max_depth)
    {
        lock->max_depth = lock->depth;
    }
}

static void counting_release(void *ctx)
{
    counting_lock *lock = (counting_lock *) ctx;
    lock->depth--;
    if (lock->depth < 0)
    {
        lock->ever_negative = true;
    }
}

typedef struct
{
    tess_map map;
    tess_fb_tiles source_state;
    tess_tile_source source;
    tess_canvas canvas;
    tess_painter painter;
    counting_lock lock_state;
    tess_lock lock;
} fixture;

static tess_status setup_with_slots(fixture *f, int zoom, uint16_t slot_count)
{
    memset(f, 0, sizeof(*f));

    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = tile_pixels[i];
    }

    tess_fb_tiles_init(&f->source_state);
    f->source = tess_fb_tile_source(&f->source_state);

    f->lock.ctx = &f->lock_state;
    f->lock.acquire = counting_acquire;
    f->lock.release = counting_release;

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = zoom;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = slots;
    cfg.slot_count = slot_count;
    cfg.marker_edge_inset = 16;
    cfg.source = &f->source;
    cfg.lock = &f->lock;

    const tess_status status = tess_map_init(&f->map, &cfg);

    tess_canvas_init(&f->canvas, framebuffer, 480, 272);
    f->painter = tess_canvas_painter(&f->canvas);
    return status;
}

static tess_status setup(fixture *f, int zoom)
{
    return setup_with_slots(f, zoom, SLOTS);
}

/* Drain the queue the way the loader thread would. Bounded so that a bug that
 * refuses to make progress fails the test rather than hanging it. */
static int drain(fixture *f, int budget)
{
    int loaded = 0;
    while (budget-- > 0)
    {
        const tess_status status = tess_map_service(&f->map);
        if (status == TESS_ERR_EMPTY)
        {
            break;
        }
        if (status == TESS_OK)
        {
            loaded++;
        }
    }
    return loaded;
}

static void test_init_validation(void)
{
    begin("init refuses a configuration that cannot work");

    tess_map map;
    tess_map_config cfg;

    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = tile_pixels[i];
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = slots;
    cfg.slot_count = SLOTS;

    CHECK_STATUS(tess_map_init(&map, &cfg), TESS_OK);
    CHECK_STATUS(tess_map_init(NULL, &cfg), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_init(&map, NULL), TESS_ERR_ARG);

    /* Too few slots for the grid is refused at start-up rather than left to
     * show up as a map that never settles. */
    tess_map_config small = cfg;
    small.slot_count = 4;
    CHECK_STATUS(tess_map_init(&map, &small), TESS_ERR_ARG);

    tess_map_config even = cfg;
    even.grid_cols = 4;
    even.grid_rows = 3;
    CHECK_STATUS(tess_map_init(&map, &even), TESS_ERR_ARG);

    tess_map_config zero = cfg;
    zero.width = 0;
    CHECK_STATUS(tess_map_init(&map, &zero), TESS_ERR_ARG);
}

static void test_init_queues_the_whole_grid(void)
{
    begin("a new map asks for every tile it needs, centre first");

    fixture f;
    CHECK_STATUS(setup(&f, 14), TESS_OK);

    CHECK_EQ_I(tess_map_pending(&f.map), tess_grid_count(&f.map.grid));

    /* The first tile served is the one under the view centre. */
    tess_tile first;
    CHECK_STATUS(tess_queue_peek(&f.map.queue, &first), TESS_OK);
    const tess_tile centre = tess_view_centre_tile(&f.map.view);
    CHECK(tess_tile_equal(first, centre));
}

static void test_service_loads_and_draw_draws(void)
{
    begin("serviced tiles are the ones that get drawn");

    fixture f;
    setup(&f, 14);

    const int expected = tess_grid_count(&f.map.grid);
    CHECK_EQ_I(drain(&f, 200), expected);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);

    const tess_map_stats stats = tess_map_get_stats(&f.map);
    CHECK_EQ_I(stats.tiles_loaded, expected);
    CHECK_EQ_I(stats.tiles_failed, 0);
    CHECK_EQ_I(stats.queue_overflows, 0);

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, expected);
    CHECK_EQ_I(f.canvas.placeholders_drawn, 0);

    /* And the frame is not blank. */
    uint8_t r = 0, g = 0, b = 0;
    tess_canvas_get(&f.canvas, 240, 136, &r, &g, &b);
    CHECK(r != 0 || g != 0 || b != 0);
}

static void test_unserviced_tiles_draw_as_placeholders(void)
{
    begin("a tile that has not arrived draws as a placeholder, not a hole");

    fixture f;
    setup(&f, 14);

    /* Nothing serviced at all. */
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, 0);
    CHECK_EQ_I(f.canvas.placeholders_drawn, tess_grid_count(&f.map.grid));

    /* Service exactly one, and one fewer placeholder is drawn. */
    CHECK_STATUS(tess_map_service(&f.map), TESS_OK);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, 1);
    CHECK_EQ_I(f.canvas.placeholders_drawn, tess_grid_count(&f.map.grid) - 1);
}

static void test_missing_tiles_are_not_retried_forever(void)
{
    begin("a tile the medium does not have is counted and let go");

    fixture f;
    setup(&f, 14);

    const tess_tile centre = tess_view_centre_tile(&f.map.view);
    tess_fb_tiles_add_missing(&f.source_state, centre);

    drain(&f, 200);

    const tess_map_stats stats = tess_map_get_stats(&f.map);
    CHECK_EQ_I(stats.tiles_missing, 1);
    CHECK_EQ_I(stats.tiles_loaded, tess_grid_count(&f.map.grid) - 1);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);

    /* The failed slot went back to the pool rather than being left reserved.
     * A load path that returns early without releasing leaks a slot per
     * failure, and the pool only ever shrinks. */
    uint16_t loading = 0;
    tess_cache_stats(&f.map.cache, NULL, &loading, NULL);
    CHECK_EQ_I(loading, 0);

    /* And nothing half-loaded is drawable. */
    CHECK_EQ_I(tess_cache_find(&f.map.cache, centre), -1);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.placeholders_drawn, 1);
}

static void test_io_errors_do_not_publish_a_partial_image(void)
{
    begin("a failed read is never committed");

    fixture f;
    setup(&f, 14);
    f.source_state.io_error_after = 3;  /* the first three succeed */

    drain(&f, 200);

    const tess_map_stats stats = tess_map_get_stats(&f.map);
    CHECK_EQ_I(stats.tiles_loaded, 3);
    CHECK_EQ_I(stats.tiles_failed, tess_grid_count(&f.map.grid) - 3);

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, 3);

    uint16_t loading = 0;
    tess_cache_stats(&f.map.cache, NULL, &loading, NULL);
    CHECK_EQ_I(loading, 0);
}

static void test_panning_only_asks_for_what_is_new(void)
{
    begin("a small pan re-uses the tiles already loaded");

    fixture f;
    setup(&f, 14);
    drain(&f, 200);

    const uint32_t before = tess_map_get_stats(&f.map).tiles_requested;

    /* Ten pixels: not enough to bring a new tile into the grid at all. */
    tess_map_pan(&f.map, 10, 0);
    CHECK_EQ_I(tess_map_get_stats(&f.map).tiles_requested, before);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);

    /* A whole tile east: one new column, one column dropped. */
    tess_map_pan(&f.map, TESSERA_TILE_SIZE, 0);
    CHECK_EQ_I(tess_map_pending(&f.map), f.map.grid.rows);

    /* Re-queueing the whole wanted set on every viewport update is the naive
     * version, and on a slow medium it means the reader spends its time
     * fetching tiles that are already in the cache. */
    CHECK(tess_map_get_stats(&f.map).tiles_requested < before + tess_grid_count(&f.map.grid));
}

static void test_a_long_pan_keeps_working(void)
{
    begin("panning a long way does not run the cache dry or lose tiles");

    fixture f;
    setup(&f, 14);

    for (int step = 0; step < 60; step++)
    {
        tess_map_pan(&f.map, 128, 64);
        drain(&f, 100);

        CHECK_EQ_I(tess_map_pending(&f.map), 0);
        CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
        /* Every position in the grid drew a real tile, every frame. */
        CHECK_EQ_I(f.canvas.tiles_drawn, tess_grid_count(&f.map.grid));
        CHECK_EQ_I(f.canvas.placeholders_drawn, 0);
    }

    const tess_map_stats stats = tess_map_get_stats(&f.map);
    CHECK_EQ_I(stats.queue_overflows, 0);
    CHECK_EQ_I(stats.cache_full, 0);
}

static void test_zoom_changes_the_tiles(void)
{
    begin("zooming asks for the new level and keeps drawing meanwhile");

    fixture f;
    setup(&f, 14);
    drain(&f, 200);

    tess_map_zoom(&f.map, +1);
    CHECK_EQ_I(f.map.view.zoom, 15);
    CHECK_EQ_I(tess_map_pending(&f.map), tess_grid_count(&f.map.grid));

    /* The old zoom's tiles are still cached, so they are not drawn -- they are
     * not the tiles the view wants -- but they have not been thrown away
     * either, which is what makes zooming back instant. */
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, 0);

    drain(&f, 200);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.tiles_drawn, tess_grid_count(&f.map.grid));

    tess_map_zoom(&f.map, -1);
    CHECK_EQ_I(f.map.view.zoom, 14);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);  /* still cached: nothing to fetch */
}

static void test_a_grid_sized_cache_cannot_retain_the_old_zoom(void)
{
    begin("a cache the size of the grid loses the level you zoomed away from");

    /* Not a defect -- an unavoidable consequence of the arithmetic, recorded
     * because it is the whole reason map.h recommends sizing the cache at
     * twice the grid. With exactly enough slots for one level, loading the
     * next level evicts the previous one tile for tile, and zooming back is a
     * full round of reads. */
    fixture f;
    CHECK_STATUS(setup_with_slots(&f, 14, 25), TESS_OK);
    CHECK_EQ_I(tess_grid_count(&f.map.grid), 25);

    drain(&f, 200);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);

    tess_map_zoom(&f.map, +1);
    drain(&f, 200);

    tess_map_zoom(&f.map, -1);
    CHECK_EQ_I(tess_map_pending(&f.map), tess_grid_count(&f.map.grid));

    /* It still works, it is just not free. */
    drain(&f, 200);
    CHECK_EQ_I(tess_map_pending(&f.map), 0);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.placeholders_drawn, 0);
}

static void test_high_latitude_does_not_reach_the_source(void)
{
    begin("tiles off the top of the world are never requested");

    fixture f;
    setup(&f, 10);

    const tess_geo far_north = {89.9, 0.0};
    tess_map_set_centre(&f.map, far_north);
    drain(&f, 200);

    /* Latitude is clamped to the projectable range, so the centre row is the
     * top one and the rows above it are simply absent -- rather than being
     * large negative numbers that get formatted into a path. */
    const tess_tile centre = tess_view_centre_tile(&f.map.view);
    CHECK_EQ_I(centre.y, 0);

    for (uint8_t col = 0; col < f.map.grid.cols; col++)
    {
        const tess_tile above = tess_grid_tile_at(&f.map.grid, centre, col, 0);
        CHECK(!tess_tile_is_valid(above));
    }

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK(f.canvas.placeholders_drawn >= f.map.grid.cols * 2);

    /* Nothing that was asked for was invalid. */
    const tess_map_stats stats = tess_map_get_stats(&f.map);
    CHECK_EQ_I(stats.tiles_failed, 0);
}

static void test_markers(void)
{
    begin("markers are set, drawn, framed and cleared");

    fixture f;
    setup(&f, 14);
    drain(&f, 200);

    CHECK_EQ_I(tess_map_marker_count(&f.map), 0);

    CHECK_STATUS(tess_map_marker_set(&f.map, TESS_MARKER_FOCUS, test_site(), "Focus"), TESS_OK);
    CHECK_STATUS(tess_map_marker_set_heading(&f.map, TESS_MARKER_FOCUS, 45), TESS_OK);
    CHECK_EQ_I(tess_map_marker_count(&f.map), 1);
    CHECK_STR_EQ(tess_map_marker(&f.map, TESS_MARKER_FOCUS)->label, "Focus");
    CHECK_EQ_I(tess_map_marker(&f.map, TESS_MARKER_FOCUS)->heading_deg, 45);

    const tess_geo nearby = {51.4900, 0.0100};
    CHECK_STATUS(tess_map_marker_set(&f.map, 1, nearby, "Waypoint"), TESS_OK);
    CHECK_EQ_I(tess_map_marker_count(&f.map), 2);

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.markers_drawn + f.canvas.arrows_drawn, 2);

    /* Hiding one stops it being drawn without shifting the other's index. */
    CHECK_STATUS(tess_map_marker_set_visible(&f.map, 1, false), TESS_OK);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.markers_drawn + f.canvas.arrows_drawn, 1);
    CHECK_STATUS(tess_map_marker_set_visible(&f.map, 1, true), TESS_OK);

    /* Out-of-range indices are refused rather than writing past the array. */
    CHECK_STATUS(tess_map_marker_set(&f.map, TESS_MARKER_MAX, test_site(), "x"), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_marker_set_heading(&f.map, 9, 0), TESS_ERR_ARG);
    CHECK(tess_map_marker(&f.map, 9) == NULL);

    tess_map_markers_clear(&f.map);
    CHECK_EQ_I(tess_map_marker_count(&f.map), 0);
    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.markers_drawn + f.canvas.arrows_drawn, 0);
}

static void test_far_markers_become_arrows(void)
{
    begin("a marker outside the viewport is drawn as an edge arrow");

    fixture f;
    setup(&f, 16);
    drain(&f, 200);

    tess_map_marker_set(&f.map, TESS_MARKER_FOCUS, test_site(), "Focus");

    /* Half a degree away at zoom 16 is tens of thousands of pixels. */
    const tess_geo distant = {test_site().latitude + 0.5, test_site().longitude};
    tess_map_marker_set(&f.map, 1, distant, "Far");

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.markers_drawn, 1);
    CHECK_EQ_I(f.canvas.arrows_drawn, 1);

    tess_marker_placement placement;
    CHECK_STATUS(tess_map_marker_placement(&f.map, 1, &placement), TESS_OK);
    CHECK(!placement.on_screen);
    CHECK_EQ_I(placement.bearing_deg, 0);  /* due north */
    CHECK_EQ_I(placement.point.y, 16);     /* on the inset top edge */
}

static void test_fit_markers(void)
{
    begin("fitting the markers brings them all on screen");

    fixture f;
    setup(&f, 16);

    const tess_geo a = {51.4600, -0.0500};
    const tess_geo b = {51.5000,  0.0300};
    tess_map_marker_set(&f.map, 0, a, "A");
    tess_map_marker_set(&f.map, 1, b, "B");

    CHECK_STATUS(tess_map_fit_markers(&f.map, 24), TESS_OK);
    drain(&f, 300);

    CHECK_STATUS(tess_map_draw(&f.map, &f.painter), TESS_OK);
    CHECK_EQ_I(f.canvas.markers_drawn, 2);
    CHECK_EQ_I(f.canvas.arrows_drawn, 0);

    /* With no visible markers there is nothing to frame. */
    tess_map_markers_clear(&f.map);
    CHECK_STATUS(tess_map_fit_markers(&f.map, 24), TESS_ERR_ARG);
}

static void test_marker_clear_retracts_the_count(void)
{
    begin("clearing the last marker stops it being iterated");

    fixture f;
    setup(&f, 14);

    tess_map_marker_set(&f.map, 0, test_site(), "A");
    tess_map_marker_set(&f.map, 1, test_site(), "B");
    tess_map_marker_set(&f.map, 2, test_site(), "C");
    CHECK_EQ_I(tess_map_marker_count(&f.map), 3);

    CHECK_STATUS(tess_map_marker_clear(&f.map, 2), TESS_OK);
    CHECK_EQ_I(tess_map_marker_count(&f.map), 2);

    /* Clearing one in the middle keeps the count, because index 1 has to stay
     * index 1. A list that compacted on removal would renumber everything
     * after it, and whoever holds index 3 would silently start updating
     * something else. */
    CHECK_STATUS(tess_map_marker_clear(&f.map, 0), TESS_OK);
    CHECK_EQ_I(tess_map_marker_count(&f.map), 2);
    CHECK(tess_map_marker(&f.map, 1) != NULL);
    CHECK_STR_EQ(tess_map_marker(&f.map, 1)->label, "B");
}

static void test_screen_to_geo(void)
{
    begin("a tap maps back to a position, for tap-to-place");

    fixture f;
    setup(&f, 15);

    const tess_point tap = {380, 90};
    const tess_geo where = tess_map_screen_to_geo(&f.map, tap);

    tess_map_marker_set(&f.map, 1, where, "Tapped");

    tess_marker_placement placement;
    CHECK_STATUS(tess_map_marker_placement(&f.map, 1, &placement), TESS_OK);
    CHECK(placement.on_screen);
    CHECK(placement.point.x >= tap.x - 1 && placement.point.x <= tap.x + 1);
    CHECK(placement.point.y >= tap.y - 1 && placement.point.y <= tap.y + 1);
}

static void test_the_lock_is_used_and_always_released(void)
{
    begin("every path takes the lock and gives it back");

    fixture f;
    setup(&f, 14);

    CHECK(f.lock_state.acquires > 0);
    CHECK_EQ_I(f.lock_state.depth, 0);

    drain(&f, 200);
    CHECK_EQ_I(f.lock_state.depth, 0);

    /* The failure paths too. A missing tile and an I/O error each go through
     * an early return, and an early return is exactly where a release gets
     * forgotten -- after which the map stops loading tiles until the unit is
     * power-cycled. */
    f.source_state.io_error_after = 1;
    tess_map_pan(&f.map, 2000, 2000);
    drain(&f, 200);
    CHECK_EQ_I(f.lock_state.depth, 0);

    tess_map_draw(&f.map, &f.painter);
    CHECK_EQ_I(f.lock_state.depth, 0);
    CHECK(!f.lock_state.ever_negative);

    /* And it is never taken recursively -- a plain, non-recursive RTOS mutex
     * has to be enough, or the port would deadlock against itself. */
    CHECK_EQ_I(f.lock_state.max_depth, 1);
}

static void test_no_source_is_a_supported_state(void)
{
    begin("a map with no tile source draws placeholders rather than failing");

    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = tile_pixels[i];
    }

    tess_map map;
    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = slots;
    cfg.slot_count = SLOTS;

    CHECK_STATUS(tess_map_init(&map, &cfg), TESS_OK);
    CHECK_STATUS(tess_map_service(&map), TESS_ERR_EMPTY);

    tess_canvas canvas;
    tess_canvas_init(&canvas, framebuffer, 480, 272);
    const tess_painter painter = tess_canvas_painter(&canvas);

    CHECK_STATUS(tess_map_draw(&map, &painter), TESS_OK);
    CHECK_EQ_I(canvas.placeholders_drawn, tess_grid_count(&map.grid));
}

static void test_null_safety(void)
{
    begin("NULL arguments are refused, not dereferenced");

    fixture f;
    setup(&f, 14);

    CHECK_STATUS(tess_map_service(NULL), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_draw(NULL, &f.painter), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_draw(&f.map, NULL), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_set_size(NULL, 100, 100), TESS_ERR_ARG);
    CHECK_STATUS(tess_map_fit_markers(NULL, 0), TESS_ERR_ARG);
    CHECK_EQ_I(tess_map_pending(NULL), 0);
    CHECK_EQ_I(tess_map_marker_count(NULL), 0);
    CHECK(tess_map_marker(NULL, 0) == NULL);
    CHECK_STATUS(tess_map_marker_placement(&f.map, 0, NULL), TESS_ERR_ARG);

    tess_map_set_centre(NULL, test_site());
    tess_map_pan(NULL, 1, 1);
    tess_map_zoom(NULL, 1);
    tess_map_refresh(NULL);
    tess_map_markers_clear(NULL);

    const tess_map_stats stats = tess_map_get_stats(NULL);
    CHECK_EQ_I(stats.tiles_loaded, 0);
}

int main(void)
{
    printf("tessera: engine\n");

    test_init_validation();
    test_init_queues_the_whole_grid();
    test_service_loads_and_draw_draws();
    test_unserviced_tiles_draw_as_placeholders();
    test_missing_tiles_are_not_retried_forever();
    test_io_errors_do_not_publish_a_partial_image();
    test_panning_only_asks_for_what_is_new();
    test_a_long_pan_keeps_working();
    test_zoom_changes_the_tiles();
    test_a_grid_sized_cache_cannot_retain_the_old_zoom();
    test_high_latitude_does_not_reach_the_source();
    test_markers();
    test_far_markers_become_arrows();
    test_fit_markers();
    test_marker_clear_retracts_the_count();
    test_screen_to_geo();
    test_the_lock_is_used_and_always_released();
    test_no_source_is_a_supported_state();
    test_null_safety();

    REPORT("map");
}
