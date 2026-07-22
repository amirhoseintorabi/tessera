/* SPDX-License-Identifier: MIT */
#include "tessera/projection.h"

#include <math.h>
#include <stddef.h>

/* asinh is C99, but some embedded libms ship without it. The identity is
 * exact enough and avoids depending on it. */
static double tess_asinh(double x)
{
    return log(x + sqrt(x * x + 1.0));
}

int32_t tess_tiles_per_axis(int zoom)
{
    if (zoom < 0)
    {
        zoom = 0;
    }
    if (zoom > 30)
    {
        zoom = 30; /* 2^31 would overflow int32_t */
    }
    return (int32_t) 1 << zoom;
}

double tess_clamp_latitude(double latitude)
{
    if (latitude > TESSERA_MAX_LATITUDE)
    {
        return TESSERA_MAX_LATITUDE;
    }
    if (latitude < -TESSERA_MAX_LATITUDE)
    {
        return -TESSERA_MAX_LATITUDE;
    }
    return latitude;
}

double tess_wrap_longitude(double longitude)
{
    /* fmod keeps this O(1) whatever the input; a while-loop is fine for normal
     * values and pathological if it is ever handed a large one. */
    double wrapped = fmod(longitude + 180.0, 360.0);
    if (wrapped < 0.0)
    {
        wrapped += 360.0;
    }
    return wrapped - 180.0;
}

bool tess_zoom_is_valid(int zoom)
{
    return zoom >= TESSERA_MIN_ZOOM && zoom <= TESSERA_MAX_ZOOM;
}

int tess_min_zoom_for_width(int32_t width_px)
{
    int zoom = TESSERA_MIN_ZOOM;

    while (zoom < TESSERA_MAX_ZOOM
           && (int64_t) tess_tiles_per_axis(zoom) * TESSERA_TILE_SIZE < (int64_t) width_px)
    {
        zoom++;
    }
    return zoom;
}

/*
 * Snap a continuous tile coordinate that is a hair off an integer onto it.
 *
 * A tile boundary is a half-open interval: a position exactly on tile N's
 * northern or western edge belongs to tile N, not to N-1. Round-tripping a
 * boundary through the projection and back does not land exactly on the
 * integer, though -- it lands a few ulp below it -- so a bare floor() returns
 * N-1, and the pixel offset within the tile comes back as 255 instead of 0.
 *
 * The symptom on a real map is a one-tile jump whenever the view centre happens
 * to sit on a boundary, and a marker that flicks to the opposite edge of the
 * screen as it crosses one.
 *
 * The tolerance has to scale with the coordinate. A double carries about 16
 * significant digits, so the round-trip error is proportional to the magnitude
 * of the value, and the value runs to 2^zoom: 65536 at zoom 16 but 4,194,304
 * at zoom 22. A fixed absolute tolerance that is generous at one end of that
 * range is below the noise floor at the other, and the coordinate then lands
 * just short of the integer and floor() takes it to the previous tile.
 *
 * 1e-12 relative, with a 1e-9 absolute floor for the low zooms where the
 * coordinate is small. At zoom 22 that is 4.2e-6 of a tile, which is one
 * thousandth of a pixel -- comfortably above the round-trip error and far
 * below anything that could move a rendered position.
 */
static double tess_snap_to_boundary(double value)
{
    const double nearest = floor(value + 0.5);
    const double scaled = fabs(nearest) * 1e-12;
    const double tolerance = (scaled > 1e-9) ? scaled : 1e-9;

    if (fabs(value - nearest) < tolerance)
    {
        return nearest;
    }
    return value;
}

void tess_geo_to_tile_f(tess_geo position, int zoom, double *out_x, double *out_y)
{
    const double n = (double) tess_tiles_per_axis(zoom);
    const double lat = tess_clamp_latitude(position.latitude);
    const double lon = tess_wrap_longitude(position.longitude);
    const double lat_rad = lat * (M_PI / 180.0);

    if (out_x != NULL)
    {
        *out_x = tess_snap_to_boundary(n * ((lon + 180.0) / 360.0));
    }
    if (out_y != NULL)
    {
        *out_y = tess_snap_to_boundary(n * (1.0 - (tess_asinh(tan(lat_rad)) / M_PI)) / 2.0);
    }
}

tess_tile tess_geo_to_tile(tess_geo position, int zoom)
{
    double fx = 0.0, fy = 0.0;
    tess_geo_to_tile_f(position, zoom, &fx, &fy);

    tess_tile tile;
    tile.x = (int32_t) floor(fx);
    tile.y = (int32_t) floor(fy);
    tile.zoom = zoom;

    /* A longitude of exactly +180 wraps to -180 and lands on tile 0, but a
     * latitude at the clamp can still land exactly on n, one past the last
     * row. Clamp rather than let an out-of-range index reach the filename
     * formatter. */
    const int32_t n = tess_tiles_per_axis(zoom);
    if (tile.x < 0)
    {
        tile.x = 0;
    }
    if (tile.x > n - 1)
    {
        tile.x = n - 1;
    }
    if (tile.y < 0)
    {
        tile.y = 0;
    }
    if (tile.y > n - 1)
    {
        tile.y = n - 1;
    }
    return tile;
}

tess_pixel tess_geo_to_pixel_in_tile(tess_geo position, int zoom)
{
    double fx = 0.0, fy = 0.0;
    tess_geo_to_tile_f(position, zoom, &fx, &fy);

    tess_pixel pixel;
    pixel.x = (int32_t) floor((fx - floor(fx)) * TESSERA_TILE_SIZE);
    pixel.y = (int32_t) floor((fy - floor(fy)) * TESSERA_TILE_SIZE);

    /* The multiply can land exactly on 256 for a position on the far edge. */
    if (pixel.x > TESSERA_TILE_SIZE - 1)
    {
        pixel.x = TESSERA_TILE_SIZE - 1;
    }
    if (pixel.y > TESSERA_TILE_SIZE - 1)
    {
        pixel.y = TESSERA_TILE_SIZE - 1;
    }
    return pixel;
}

tess_geo tess_tile_f_to_geo(double x, double y, int zoom)
{
    const double n = (double) tess_tiles_per_axis(zoom);
    tess_geo position;

    position.longitude = (x / n) * 360.0 - 180.0;

    const double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * y / n)));
    position.latitude = lat_rad * (180.0 / M_PI);

    return position;
}

tess_geo tess_tile_north_west(tess_tile tile)
{
    /* An integer tile address is a continuous coordinate with no fractional
     * part, so this is the same transform rather than a second copy of it.
     * Two copies of a projection drift apart the first time one is corrected. */
    return tess_tile_f_to_geo((double) tile.x, (double) tile.y, tile.zoom);
}

tess_bounds tess_tile_bounds(tess_tile tile)
{
    tess_tile south_east = tile;
    south_east.x += 1;
    south_east.y += 1;

    const tess_geo nw = tess_tile_north_west(tile);
    const tess_geo se = tess_tile_north_west(south_east);

    tess_bounds bounds;
    bounds.south_west.latitude = se.latitude;
    bounds.south_west.longitude = nw.longitude;
    bounds.north_east.latitude = nw.latitude;
    bounds.north_east.longitude = se.longitude;
    return bounds;
}

void tess_pixel_delta(tess_geo from, tess_geo to, int zoom, int32_t *out_dx, int32_t *out_dy)
{
    double fx = 0.0, fy = 0.0, tx = 0.0, ty = 0.0;
    tess_geo_to_tile_f(from, zoom, &fx, &fy);
    tess_geo_to_tile_f(to, zoom, &tx, &ty);

    if (out_dx != NULL)
    {
        /*
         * Take the short way round.
         *
         * Longitude wraps, so two points either side of the antimeridian are a
         * few pixels apart on the ground and almost a whole world apart in
         * tile coordinates. Without this a marker just west of 180 degrees is
         * placed off the far edge of a viewport centred just east of it --
         * visible as a marker that jumps the width of the world as the map
         * crosses the line.
         */
        const double n = (double) tess_tiles_per_axis(zoom);
        double dx = tx - fx;

        if (dx > n / 2.0)
        {
            dx -= n;
        }
        else if (dx < -n / 2.0)
        {
            dx += n;
        }

        *out_dx = (int32_t) lround(dx * TESSERA_TILE_SIZE);
    }
    if (out_dy != NULL)
    {
        /* +y is south in tile space and on the screen, so no sign flip. */
        *out_dy = (int32_t) lround((ty - fy) * TESSERA_TILE_SIZE);
    }
}

double tess_metres_per_pixel(double latitude, int zoom)
{
    /* Equatorial circumference / (tile size * tiles per axis), narrowed by the
     * cosine of the latitude. */
    const double equator_m = 40075016.686;
    const double lat_rad = tess_clamp_latitude(latitude) * (M_PI / 180.0);
    const double px_per_axis = (double) TESSERA_TILE_SIZE * (double) tess_tiles_per_axis(zoom);
    return equator_m * cos(lat_rad) / px_per_axis;
}

bool tess_bounds_of(const tess_geo *positions, int count, tess_bounds *out)
{
    if (positions == NULL || out == NULL || count < 1)
    {
        return false;
    }

    double min_lat = positions[0].latitude, max_lat = positions[0].latitude;
    double min_lon = positions[0].longitude, max_lon = positions[0].longitude;

    for (int i = 1; i < count; i++)
    {
        if (positions[i].latitude < min_lat)
        {
            min_lat = positions[i].latitude;
        }
        if (positions[i].latitude > max_lat)
        {
            max_lat = positions[i].latitude;
        }
        if (positions[i].longitude < min_lon)
        {
            min_lon = positions[i].longitude;
        }
        if (positions[i].longitude > max_lon)
        {
            max_lon = positions[i].longitude;
        }
    }

    out->south_west.latitude = min_lat;
    out->south_west.longitude = min_lon;
    out->north_east.latitude = max_lat;
    out->north_east.longitude = max_lon;
    return true;
}

tess_geo tess_bounds_centre(tess_bounds bounds)
{
    tess_geo centre;
    centre.latitude = (bounds.south_west.latitude + bounds.north_east.latitude) / 2.0;
    centre.longitude = (bounds.south_west.longitude + bounds.north_east.longitude) / 2.0;
    return centre;
}

int tess_zoom_to_fit(tess_bounds bounds, int width_px, int height_px)
{
    if (width_px <= 0 || height_px <= 0)
    {
        return TESSERA_MIN_ZOOM;
    }

    /* Walk down from the tightest zoom and take the first that fits. Only
     * seven levels, so a search is not worth the closed form -- and the closed
     * form has to special-case a zero-extent box, which a caller hits the
     * moment it asks to fit a single point. */
    for (int zoom = TESSERA_MAX_ZOOM; zoom > TESSERA_MIN_ZOOM; zoom--)
    {
        int32_t dx = 0, dy = 0;
        tess_pixel_delta(bounds.south_west, bounds.north_east, zoom, &dx, &dy);

        const int32_t span_x = dx < 0 ? -dx : dx;
        const int32_t span_y = dy < 0 ? -dy : dy;

        if (span_x <= (int32_t) width_px && span_y <= (int32_t) height_px)
        {
            return zoom;
        }
    }
    return TESSERA_MIN_ZOOM;
}
