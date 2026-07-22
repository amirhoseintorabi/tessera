/* SPDX-License-Identifier: MIT */
/*
 * Renders a few frames of the widget to PPM files.
 *
 * This is what makes the rest of the library checkable by eye. Assertions
 * catch a tile in the wrong place only if someone thought to assert on that
 * tile; an image catches it because a map with one row displaced looks wrong
 * immediately. A pan, a zoom about an anchor, an off-screen marker and a
 * missing tile each come out as a file you can open.
 *
 * It runs as part of the test suite because a demo nobody builds is a demo
 * that has quietly stopped compiling. The frames only appear when an output
 * directory is given on the command line.
 */

#include "framebuffer.h"

#include <stdio.h>
#include <string.h>

#define WIDTH  480   /* a common small-panel size */
#define HEIGHT 272
#define SLOTS  25

/* The Royal Observatory, Greenwich -- a fixed, public reference point. On the
 * prime meridian, so a longitude sign error shows up at once. */
static const tess_geo kSite = {51.47788, -0.00159};

static uint16_t tile_pixels[SLOTS][TESSERA_TILE_SIZE * TESSERA_TILE_SIZE];
static tess_slot slots[SLOTS];
static uint8_t framebuffer[WIDTH * HEIGHT * 3];

/* Drain the queue the way the loader thread would, bounded so a defect that
 * stops making progress ends the program rather than hanging it. */
static void load_everything(tess_map *map)
{
    for (int i = 0; i < 500; i++)
    {
        if (tess_map_service(map) == TESS_ERR_EMPTY)
        {
            return;
        }
    }
}

static int render(tess_map *map, tess_canvas *canvas, const tess_painter *painter,
                  const char *out_dir, const char *name)
{
    load_everything(map);

    if (tess_map_draw(map, painter) != TESS_OK)
    {
        fprintf(stderr, "demo: draw failed for %s\n", name);
        return 1;
    }

    printf("  %-22s zoom %2d  tiles %2u  placeholders %u  markers %u  arrows %u\n",
           name, map->view.zoom, canvas->tiles_drawn, canvas->placeholders_drawn,
           canvas->markers_drawn, canvas->arrows_drawn);

    if (out_dir == NULL)
    {
        return 0;
    }

    char path[512];
    if (snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name) >= (int) sizeof path)
    {
        fprintf(stderr, "demo: output path too long\n");
        return 1;
    }
    if (tess_canvas_write_ppm(canvas, path) != TESS_OK)
    {
        fprintf(stderr, "demo: could not write %s\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_dir = (argc > 1) ? argv[1] : NULL;

    for (int i = 0; i < SLOTS; i++)
    {
        slots[i].image = tile_pixels[i];
    }

    tess_fb_tiles source_state;
    tess_fb_tiles_init(&source_state);
    const tess_tile_source source = tess_fb_tile_source(&source_state);

    tess_map_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.centre = kSite;
    cfg.zoom = 15;
    cfg.width = WIDTH;
    cfg.height = HEIGHT;
    cfg.slots = slots;
    cfg.slot_count = SLOTS;
    cfg.marker_edge_inset = 16;
    cfg.source = &source;

    tess_map map;
    if (tess_map_init(&map, &cfg) != TESS_OK)
    {
        fprintf(stderr, "demo: map init failed\n");
        return 1;
    }

    tess_canvas canvas;
    tess_canvas_init(&canvas, framebuffer, WIDTH, HEIGHT);
    const tess_painter painter = tess_canvas_painter(&canvas);

    tess_map_marker_set(&map, TESS_MARKER_FOCUS, kSite, "Focus");
    tess_map_marker_set_heading(&map, TESS_MARKER_FOCUS, 45);

    const tess_geo nearby = {kSite.latitude + 0.0018, kSite.longitude + 0.0035};
    tess_map_marker_set(&map, 1, nearby, "Waypoint");

    printf("tessera demo, %dx%d\n", WIDTH, HEIGHT);
    int failures = 0;

    failures += render(&map, &canvas, &painter, out_dir, "01-initial");

    tess_map_pan(&map, 180, 110);
    failures += render(&map, &canvas, &painter, out_dir, "02-panned");

    /* Zoom out about the top-right corner rather than the middle, which is
     * the case a centre-only zoom cannot express. */
    const tess_point anchor = {WIDTH - 40, 40};
    tess_map_zoom_at(&map, -2, anchor);
    failures += render(&map, &canvas, &painter, out_dir, "03-zoomed-out");

    /* A third marker far enough out that it cannot be shown in place, so the
     * frame contains an edge arrow pointing at it. */
    tess_map_zoom(&map, +4);
    tess_map_set_centre(&map, kSite);

    const tess_geo depot = {kSite.latitude + 0.030, kSite.longitude - 0.012};
    tess_map_marker_set(&map, 2, depot, "Remote");
    failures += render(&map, &canvas, &painter, out_dir, "04-arrow");

    /* Frame both markers. */
    if (tess_map_fit_markers(&map, 32) != TESS_OK)
    {
        fprintf(stderr, "demo: fit failed\n");
        failures++;
    }
    failures += render(&map, &canvas, &painter, out_dir, "05-fitted");

    /* Missing tiles, so the placeholder path shows up in an image too. */
    const tess_tile centre = tess_view_centre_tile(&map.view);
    tess_fb_tiles_add_missing(&source_state, centre);
    tess_tile east = centre;
    east.x += 1;
    tess_fb_tiles_add_missing(&source_state, east);
    tess_cache_reset(&map.cache);
    tess_map_refresh(&map);
    failures += render(&map, &canvas, &painter, out_dir, "06-missing-tiles");

    const tess_map_stats stats = tess_map_get_stats(&map);
    printf("\n  requested %u  loaded %u  missing %u  failed %u  overflows %u  cache-full %u\n",
           stats.tiles_requested, stats.tiles_loaded, stats.tiles_missing,
           stats.tiles_failed, stats.queue_overflows, stats.cache_full);

    if (out_dir != NULL)
    {
        printf("  frames written to %s\n", out_dir);
    }
    return failures ? 1 : 0;
}
