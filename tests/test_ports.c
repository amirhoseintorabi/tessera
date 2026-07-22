/* SPDX-License-Identifier: MIT */
/*
 * Tests for the three target ports, run against the interoperability
 * declarations under each port's interop/ directory.
 *
 * These exercise the same source files that get compiled into firmware. The
 * declarations are not the real FatFs, CMSIS-RTOS or emWin, so what is
 * established here is the port's own logic -- its bounds checks, its error
 * handling, its balance of acquire and release -- and not the behaviour of the
 * libraries underneath. That distinction is worth keeping in mind while
 * reading what follows; it is set out in port/emwin/interop/README.md and in
 * docs/EMWIN.md.
 */

#include "tessera_emwin.h"
#include "framebuffer.h"
#include "tessera_rtos.h"
#include "tessera_fatfs.h"

#include "emwin_probe.h"

#include "check.h"
#include "fixture.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SLOTS 30
#define TILE_BYTES TESS_TILE_BYTES_16BPP


static uint8_t tile_buffers[SLOTS][TILE_BYTES];
static tess_slot slots[SLOTS];

static char tile_dir[256];

/* ------------------------------------------------------- FatFs tile source */

/* A flat pattern, so the test does not have to create a z/x/y tree. */
#define TEST_PATTERN "t_{z}_{x}_{y}.bin"

/* Write a tile file of `bytes` bytes for `tile` into the temporary directory. */
static bool write_tile_file(tess_tile tile, size_t bytes, uint8_t fill)
{
    char name[TESS_TILE_PATH_MAX];
    if (tess_tile_path(name, sizeof name, TEST_PATTERN, tile) != TESS_OK)
    {
        return false;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/%s", tile_dir, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < bytes; i++)
    {
        fputc(fill, f);
    }
    return fclose(f) == 0;
}

static tess_fatfs_tiles make_fatfs_state(char *pattern, size_t pattern_size)
{
    snprintf(pattern, pattern_size, "%s/" TEST_PATTERN, tile_dir);

    tess_fatfs_tiles state;
    memset(&state, 0, sizeof(state));
    state.pattern = pattern;
    state.image_capacity = TILE_BYTES;
    state.minimum_size = 64;
    return state;
}

static void test_fatfs_reads_a_tile(void)
{
    begin("the FatFs source reads a well-formed tile");

    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);
    CHECK(source.load != NULL);

    const tess_tile tile = T(1000, 500, 14);
    CHECK(write_tile_file(tile, TILE_BYTES, 0xA5));

    memset(tile_buffers[0], 0, TILE_BYTES);
    CHECK_STATUS(source.load(source.ctx, tile, tile_buffers[0]), TESS_OK);
    CHECK_EQ_I(state.reads_ok, 1);
    CHECK_EQ_I(tile_buffers[0][0], 0xA5);
    CHECK_EQ_I(tile_buffers[0][TILE_BYTES - 1], 0xA5);
}

static void test_fatfs_missing_tile(void)
{
    begin("a tile the card does not have is TESS_ERR_NOT_FOUND, not an error");

    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);

    CHECK_STATUS(source.load(source.ctx, T(9999, 9999, 14), tile_buffers[0]), TESS_ERR_NOT_FOUND);
    CHECK_EQ_I(state.not_found, 1);
    CHECK_EQ_I(state.io_errors, 0);
}

static void test_fatfs_refuses_an_oversized_tile(void)
{
    begin("a tile file larger than the slot is refused, not read into it");

    /* The length comes from the file and the destination is fixed, so the two
     * have to be compared. Removable storage makes this untrusted input: its
     * contents are chosen by whoever last held the card.
     *
     * The assertion is that the read is refused. Under ASan a regression would
     * additionally show up as a heap overflow, since the destination buffer is
     * exactly TILE_BYTES. */
    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);

    const tess_tile tile = T(1001, 500, 14);
    CHECK(write_tile_file(tile, TILE_BYTES + 4096, 0x5A));

    CHECK_STATUS(source.load(source.ctx, tile, tile_buffers[0]), TESS_ERR_RANGE);
    CHECK_EQ_I(state.too_large, 1);
    CHECK_EQ_I(state.reads_ok, 0);
}

static void test_fatfs_refuses_a_truncated_tile(void)
{
    begin("a stub of a file is refused rather than decoded");

    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);

    const tess_tile tiny = T(1002, 500, 14);
    CHECK(write_tile_file(tiny, 8, 0x00));
    CHECK_STATUS(source.load(source.ctx, tiny, tile_buffers[0]), TESS_ERR_RANGE);

    const tess_tile empty = T(1003, 500, 14);
    CHECK(write_tile_file(empty, 0, 0x00));
    CHECK_STATUS(source.load(source.ctx, empty, tile_buffers[0]), TESS_ERR_RANGE);

    CHECK_EQ_I(state.reads_ok, 0);
    CHECK_EQ_I(state.too_large, 2);
}

static void test_fatfs_refuses_impossible_addresses(void)
{
    begin("an impossible tile address never reaches the filesystem");

    /* An unclamped projection produces rows like this, and without the
     * validity check they would be formatted into a path and handed to
     * f_stat. */
    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);

    CHECK_STATUS(source.load(source.ctx, T(2048, -8166, 12), tile_buffers[0]), TESS_ERR_ARG);
    CHECK_STATUS(source.load(source.ctx, T(0, 0, 42), tile_buffers[0]), TESS_ERR_ARG);
    CHECK_EQ_I(state.not_found, 0);  /* it never got as far as looking */
}

static void test_fatfs_inert_without_a_capacity(void)
{
    begin("a source with no declared capacity is handed back inert");

    /* There is no safe way to bound a read without knowing the destination
     * size, so the source refuses to exist rather than guessing. The engine
     * treats a NULL load as "no tiles" and draws placeholders. */
    tess_fatfs_tiles state;
    memset(&state, 0, sizeof(state));
    state.pattern = TEST_PATTERN;
    state.image_capacity = 0;

    const tess_tile_source source = tess_fatfs_tile_source(&state);
    CHECK(source.load == NULL);

    const tess_tile_source none = tess_fatfs_tile_source(NULL);
    CHECK(none.load == NULL);
}

static void test_fatfs_drives_the_engine(void)
{
    begin("the engine loads a screen of tiles through the FatFs source");

    char dir[300];
    tess_fatfs_tiles state = make_fatfs_state(dir, sizeof dir);
    const tess_tile_source source = tess_fatfs_tile_source(&state);

    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = tile_buffers[i];
    }

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = slots;
    cfg.slot_count = SLOTS;
    cfg.source = &source;

    tess_map map;
    CHECK_STATUS(tess_map_init(&map, &cfg), TESS_OK);

    /* Write files for exactly the tiles the view wants, so the whole grid can
     * be satisfied. */
    const tess_tile centre = tess_view_centre_tile(&map.view);
    for (uint8_t row = 0; row < map.grid.rows; row++)
    {
        for (uint8_t col = 0; col < map.grid.cols; col++)
        {
            const tess_tile tile = tess_grid_tile_at(&map.grid, centre, col, row);
            if (tess_tile_is_valid(tile))
            {
                CHECK(write_tile_file(tile, TILE_BYTES, (uint8_t)(col * 16u + row)));
            }
        }
    }

    for (int i = 0; i < 200 && tess_map_service(&map) != TESS_ERR_EMPTY; i++)
    {
        /* drain */
    }

    CHECK_EQ_I(tess_map_pending(&map), 0);
    CHECK_EQ_I(tess_map_get_stats(&map).tiles_loaded, tess_grid_count(&map.grid));
    CHECK_EQ_I(state.reads_ok, tess_grid_count(&map.grid));
    CHECK_EQ_I(state.io_errors, 0);
}

/* -------------------------------------------------------- CMSIS-RTOS loader */

typedef struct
{
    tess_rtos_loader loader;
    tess_map map;
    tess_fb_tiles source_state;
    tess_tile_source source;
} threaded;

static void *loader_entry(void *arg)
{
    tess_rtos_loader_run((tess_rtos_loader *) arg);
    return NULL;
}

static void test_rtos_loader_runs_alongside_the_gui(void)
{
    begin("the loader thread and the GUI thread share the map without seizing");

    /* The real loop, against a real mutex, while another thread pans. That is
     * as close as a host test gets to the contention the lock exists for --
     * and under -fsanitize=thread, closer still. */
    threaded *t = calloc(1, sizeof(*t));
    CHECK(t != NULL);
    if (t == NULL)
    {
        return;
    }

    tess_fb_tiles_init(&t->source_state);
    t->source = tess_fb_tile_source(&t->source_state);

    CHECK_STATUS(tess_rtos_loader_init(&t->loader, 1), TESS_OK);
    const tess_lock lock = tess_rtos_lock(&t->loader);

    static uint16_t pixels[SLOTS][TESSERA_TILE_SIZE * TESSERA_TILE_SIZE];
    static tess_slot rtos_slots[SLOTS];
    for (int i = 0; i < SLOTS; i++)
    {
        rtos_slots[i].image = pixels[i];
    }

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = rtos_slots;
    cfg.slot_count = SLOTS;
    cfg.source = &t->source;
    cfg.lock = &lock;

    CHECK_STATUS(tess_map_init(&t->map, &cfg), TESS_OK);
    t->loader.map = &t->map;

    pthread_t thread;
    CHECK_EQ_I(pthread_create(&thread, NULL, loader_entry, &t->loader), 0);

    /* Pan from this thread while the loader works, which is exactly the
     * contention the lock exists for. */
    for (int i = 0; i < 400; i++)
    {
        tess_map_pan(&t->map, 8, 4);
        (void) tess_map_pending(&t->map);
    }

    tess_rtos_loader_stop(&t->loader);
    CHECK_EQ_I(pthread_join(thread, NULL), 0);

    /* It made progress rather than deadlocking, and nothing was left
     * reserved. */
    CHECK(tess_map_get_stats(&t->map).tiles_loaded > 0);

    uint16_t loading = 0;
    tess_cache_stats(&t->map.cache, NULL, &loading, NULL);
    CHECK_EQ_I(loading, 0);

    tess_rtos_loader_deinit(&t->loader);
    free(t);
}

static void test_rtos_loader_validation(void)
{
    begin("the loader refuses to run without a map, and checks its mutex");

    tess_rtos_loader loader;
    CHECK_STATUS(tess_rtos_loader_init(NULL, 10), TESS_ERR_ARG);
    CHECK_STATUS(tess_rtos_loader_init(&loader, 10), TESS_OK);
    CHECK(loader.mutex != NULL);

    /* No map: returns immediately rather than spinning. */
    loader.map = NULL;
    tess_rtos_loader_run(&loader);
    CHECK(!loader.running);

    tess_rtos_loader_stop(NULL);

    /* Teardown releases the mutex and leaves the loader inert. On a unit where
     * the map lives from boot to power-off this never runs, which is why it
     * was easy to leave out -- but a map screen that can be opened and closed
     * would otherwise leak one RTOS mutex per visit, and the pool is small and
     * fixed. Under ASan the omission showed up as a leak. */
    tess_rtos_loader_deinit(&loader);
    CHECK(loader.mutex == NULL);
    CHECK(loader.map == NULL);

    /* Idempotent, and safe on NULL: teardown paths get called twice. */
    tess_rtos_loader_deinit(&loader);
    tess_rtos_loader_deinit(NULL);

    /* The lock vtable a map may still hold becomes a no-op rather than a
     * use-after-free. */
    const tess_lock stale = tess_rtos_lock(&loader);
    stale.acquire(stale.ctx);
    stale.release(stale.ctx);
}

/* -------------------------------------------------------------- emWin widget */

static tess_map widget_map;
static uint16_t widget_pixels[SLOTS][TESSERA_TILE_SIZE * TESSERA_TILE_SIZE];
static tess_slot widget_slots[SLOTS];
static tess_fb_tiles widget_source_state;
static tess_tile_source widget_source;
static uint32_t rotation_scratch[32 * 32];

static WM_HWIN create_widget(bool with_artwork, GUI_BITMAP *artwork)
{
    tess_fb_tiles_init(&widget_source_state);
    widget_source = tess_fb_tile_source(&widget_source_state);

    for (int i = 0; i < SLOTS; i++)
    {
        widget_slots[i].image = widget_pixels[i];
    }

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = widget_slots;
    cfg.slot_count = SLOTS;
    cfg.marker_edge_inset = 16;
    cfg.source = &widget_source;

    if (tess_map_init(&widget_map, &cfg) != TESS_OK)
    {
        return 0;
    }

    TESSERA_CONFIG wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.map = &widget_map;
    wcfg.refresh_ms = 100;
    wcfg.touch_pan = true;
    wcfg.rotation_buffer = rotation_scratch;
    wcfg.rotation_size = 32;
    if (with_artwork)
    {
        memset(artwork, 0, sizeof(*artwork));
        artwork->XSize = 31;
        artwork->YSize = 31;
        artwork->BitsPerPixel = 32;
        wcfg.focus = artwork;
        wcfg.pin = artwork;
        wcfg.arrow = artwork;
    }

    return TESSERA_CreateEx(&wcfg, 0, 0, 0, 480, 272, GUI_ID_USER + 1);
}

static void test_emwin_create(void)
{
    begin("the widget is created, takes a timer, and hands its map back");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);
    CHECK(window != 0);

    const tess_emwin_probe record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.windows_created, 1);
    CHECK_EQ_I(record.timers_created, 1);
    CHECK(record.invalidations > 0);

    CHECK(TESSERA_GetMap(window) == &widget_map);
    CHECK(TESSERA_GetMap(0) == NULL);

    /* Bad configurations produce no window rather than a broken one. */
    TESSERA_CONFIG empty;
    memset(&empty, 0, sizeof(empty));
    CHECK_EQ_I(TESSERA_CreateEx(NULL, 0, 0, 0, 100, 100, 1), 0);
    CHECK_EQ_I(TESSERA_CreateEx(&empty, 0, 0, 0, 100, 100, 1), 0);

    empty.map = &widget_map;
    CHECK_EQ_I(TESSERA_CreateEx(&empty, 0, 0, 0, 0, 100, 1), 0);
}

static void test_emwin_paints_every_tile(void)
{
    begin("WM_PAINT draws one bitmap per loaded tile and a placeholder for the rest");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);
    CHECK(window != 0);

    const int tiles = tess_grid_count(&widget_map.grid);

    /* Nothing loaded yet: every position is a placeholder. */
    tess_emwin_probe_send(window, WM_PAINT, NULL);
    tess_emwin_probe record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.rects_cleared, tiles);
    CHECK_EQ_I(record.strings_drawn, tiles);
    CHECK_EQ_I(record.streams_parsed, 0);

    /* Load everything, and now every position is a bitmap. */
    for (int i = 0; i < 200 && tess_map_service(&widget_map) != TESS_ERR_EMPTY; i++)
    {
        /* drain */
    }

    tess_emwin_probe_reset_counters();
    tess_emwin_probe_send(window, WM_PAINT, NULL);
    record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.streams_parsed, tiles);
    CHECK_EQ_I(record.bitmaps_drawn, tiles);
    CHECK_EQ_I(record.rects_cleared, 0);
}

static void test_emwin_offsets_by_the_window_origin(void)
{
    begin("drawing is offset by the window's position on screen");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);
    for (int i = 0; i < 200 && tess_map_service(&widget_map) != TESS_ERR_EMPTY; i++)
    {
        /* drain */
    }

    /* At the origin first, then moved, and the difference must be exactly the
     * offset. emWin draws in screen coordinates while the engine works in
     * widget-relative ones; getting that wrong puts the map in the right place
     * only when the widget happens to be at 0,0. */
    tess_emwin_probe_set_origin(window, 0, 0);
    tess_emwin_probe_send(window, WM_PAINT, NULL);
    tess_emwin_probe at_origin = tess_emwin_probe_stats();

    tess_emwin_probe_set_origin(window, 37, 91);
    tess_emwin_probe_send(window, WM_PAINT, NULL);
    const tess_emwin_probe moved = tess_emwin_probe_stats();

    CHECK_EQ_I(moved.last_bitmap_x - at_origin.last_bitmap_x, 37);
    CHECK_EQ_I(moved.last_bitmap_y - at_origin.last_bitmap_y, 91);
}

static void test_emwin_marker_rotation_balances(void)
{
    begin("marker rotation creates and destroys its memory devices in pairs");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);

    tess_map_marker_set(&widget_map, TESS_MARKER_FOCUS, test_site(), "Focus");
    tess_map_marker_set_heading(&widget_map, TESS_MARKER_FOCUS, 45);

    const tess_geo distant = {test_site().latitude + 0.5, test_site().longitude};
    tess_map_marker_set(&widget_map, 1, distant, "Remote");

    for (int frame = 0; frame < 20; frame++)
    {
        tess_emwin_probe_send(window, WM_PAINT, NULL);
    }

    const tess_emwin_probe record = tess_emwin_probe_stats();

    /* One focus rotated to its heading and one edge arrow rotated to its
     * bearing, twenty times over. */
    CHECK_EQ_I(record.rotations, 40);

    /* Every device created was destroyed. Rotation is the only place this
     * binding allocates anything, so an imbalance here exhausts the emWin heap
     * at a rate of two per marker per frame. */
    CHECK_EQ_I(record.memdevs_created, record.memdevs_deleted);
    CHECK_EQ_I(record.memdevs_created, 80);

    /* And never more than two alive at once -- one rotation at a time, one
     * shared scratch buffer, however many markers there are. */
    CHECK_EQ_I(record.peak_memdevs, 2);
}

static void test_emwin_without_artwork(void)
{
    begin("a widget with no artwork still draws, so a board can be brought up");

    tess_emwin_probe_reset();

    const WM_HWIN window = create_widget(false, NULL);
    CHECK(window != 0);

    tess_map_marker_set(&widget_map, TESS_MARKER_FOCUS, test_site(), "Focus");
    tess_map_marker_set_heading(&widget_map, TESS_MARKER_FOCUS, 90);

    tess_emwin_probe_send(window, WM_PAINT, NULL);
    const tess_emwin_probe record = tess_emwin_probe_stats();

    /* No bitmap to rotate, so it falls back to a plain circle rather than
     * rotating nothing and drawing an empty square. */
    CHECK_EQ_I(record.rotations, 0);
    CHECK_EQ_I(record.circles_drawn, 1);
}

static void test_emwin_touch_pans(void)
{
    begin("dragging moves the map the same way the finger went");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);

    const tess_geo before = widget_map.view.centre;

    /* The first press only records where the finger landed. */
    GUI_PID_STATE down = {200, 100, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &down);
    CHECK_NEAR(widget_map.view.centre.longitude, before.longitude, 1e-12);

    /* Dragging east moves the content east, so the view moves west. */
    GUI_PID_STATE drag = {260, 100, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &drag);
    CHECK(widget_map.view.centre.longitude < before.longitude);

    /* Dragging down moves the view north. */
    const double after_x = widget_map.view.centre.longitude;
    GUI_PID_STATE drag2 = {260, 140, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &drag2);
    CHECK(widget_map.view.centre.latitude > before.latitude);
    CHECK_NEAR(widget_map.view.centre.longitude, after_x, 1e-12);

    /* Lifting ends the drag, and the next press somewhere else does not make
     * the map jump the distance between them. */
    GUI_PID_STATE up = {260, 140, 0, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &up);

    const tess_geo settled = widget_map.view.centre;
    GUI_PID_STATE elsewhere = {40, 240, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &elsewhere);
    CHECK_NEAR(widget_map.view.centre.latitude, settled.latitude, 1e-12);
    CHECK_NEAR(widget_map.view.centre.longitude, settled.longitude, 1e-12);

    /* A NULL payload -- the pointer leaving the window -- also ends it. */
    tess_emwin_probe_send(window, WM_TOUCH, NULL);
    GUI_PID_STATE far_away = {470, 10, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &far_away);
    CHECK_NEAR(widget_map.view.centre.latitude, settled.latitude, 1e-12);
}

static void test_emwin_touch_can_be_disabled(void)
{
    begin("touch panning is opt-in");

    tess_emwin_probe_reset();
    tess_fb_tiles_init(&widget_source_state);
    widget_source = tess_fb_tile_source(&widget_source_state);

    for (int i = 0; i < SLOTS; i++)
    {
        widget_slots[i].image = widget_pixels[i];
    }

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = test_site();
    cfg.zoom = 14;
    cfg.width = 480;
    cfg.height = 272;
    cfg.slots = widget_slots;
    cfg.slot_count = SLOTS;
    cfg.source = &widget_source;
    CHECK_STATUS(tess_map_init(&widget_map, &cfg), TESS_OK);

    TESSERA_CONFIG wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.map = &widget_map;
    wcfg.touch_pan = false;

    const WM_HWIN window = TESSERA_CreateEx(&wcfg, 0, 0, 0, 480, 272, 1);
    CHECK(window != 0);

    const tess_geo before = widget_map.view.centre;
    GUI_PID_STATE down = {200, 100, 1, 0};
    GUI_PID_STATE drag = {300, 200, 1, 0};
    tess_emwin_probe_send(window, WM_TOUCH, &down);
    tess_emwin_probe_send(window, WM_TOUCH, &drag);

    CHECK_NEAR(widget_map.view.centre.latitude, before.latitude, 1e-12);
    CHECK_NEAR(widget_map.view.centre.longitude, before.longitude, 1e-12);
}

static void test_emwin_timer_only_repaints_when_something_changed(void)
{
    begin("the refresh timer does not repaint a map that is already complete");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);

    /* Tiles outstanding: the timer restarts and asks for a repaint. */
    tess_emwin_probe_reset_counters();
    tess_emwin_probe_send(window, WM_TIMER, NULL);
    tess_emwin_probe record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.timers_restarted, 1);
    CHECK_EQ_I(record.invalidations, 1);

    /* Everything loaded: the timer restarts but asks for nothing. Repainting
     * on every tick regardless is a screenful of blits per tick that nobody
     * asked for, competing with the loader for the memory bus. */
    for (int i = 0; i < 200 && tess_map_service(&widget_map) != TESS_ERR_EMPTY; i++)
    {
        /* drain */
    }

    tess_emwin_probe_reset_counters();
    tess_emwin_probe_send(window, WM_TIMER, NULL);
    record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.timers_restarted, 1);
    CHECK_EQ_I(record.invalidations, 0);
}

static void test_emwin_delete_releases_the_timer(void)
{
    begin("deleting the window deletes its timer");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);

    WM_DeleteWindow(window);

    const tess_emwin_probe record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.timers_deleted, 1);
    CHECK_EQ_I(record.windows_deleted, 1);
}

static void test_emwin_unknown_messages_go_to_the_default(void)
{
    begin("messages the widget does not handle reach WM_DefaultProc");

    tess_emwin_probe_reset();

    GUI_BITMAP artwork;
    const WM_HWIN window = create_widget(true, &artwork);

    tess_emwin_probe_reset_counters();
    tess_emwin_probe_send(window, WM_POST_PAINT, NULL);
    tess_emwin_probe_send(window, WM_GET_ID, NULL);

    const tess_emwin_probe record = tess_emwin_probe_stats();
    CHECK_EQ_I(record.default_proc_calls, 2);
}

/* ------------------------------------------------------------------- main - */

static bool make_tile_dir(void)
{
    snprintf(tile_dir, sizeof tile_dir, "tessera-tiles-XXXXXX");
    return mkdtemp(tile_dir) != NULL;
}

static void remove_tile_dir(void)
{
    /* The files are all in one flat directory with names this program chose,
     * so a shell-free sweep is possible -- but not worth the code. The
     * directory is created under the current working directory, which ctest
     * puts in the build tree. */
    char command[512];
    snprintf(command, sizeof command, "rm -rf '%s'", tile_dir);
    if (system(command) != 0)
    {
        printf("  note: could not remove %s\n", tile_dir);
    }
}

int main(void)
{
    printf("tessera: FatFs, CMSIS-RTOS and emWin ports\n");

    if (!make_tile_dir())
    {
        printf("could not create a temporary directory for tile files\n");
        return 1;
    }

    test_fatfs_reads_a_tile();
    test_fatfs_missing_tile();
    test_fatfs_refuses_an_oversized_tile();
    test_fatfs_refuses_a_truncated_tile();
    test_fatfs_refuses_impossible_addresses();
    test_fatfs_inert_without_a_capacity();
    test_fatfs_drives_the_engine();

    test_rtos_loader_validation();
    test_rtos_loader_runs_alongside_the_gui();

    test_emwin_create();
    test_emwin_paints_every_tile();
    test_emwin_offsets_by_the_window_origin();
    test_emwin_marker_rotation_balances();
    test_emwin_without_artwork();
    test_emwin_touch_pans();
    test_emwin_touch_can_be_disabled();
    test_emwin_timer_only_repaints_when_something_changed();
    test_emwin_delete_releases_the_timer();
    test_emwin_unknown_messages_go_to_the_default();

    remove_tile_dir();

    REPORT("ports");
}
