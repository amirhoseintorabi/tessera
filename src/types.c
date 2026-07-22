/* SPDX-License-Identifier: MIT */

#include "tessera/types.h"

#include <stdio.h>
#include <string.h>

const char *tess_status_name(tess_status status)
{
    switch (status)
    {
    case TESS_OK:            return "TESS_OK";
    case TESS_ERR_ARG:       return "TESS_ERR_ARG";
    case TESS_ERR_FULL:      return "TESS_ERR_FULL";
    case TESS_ERR_EMPTY:     return "TESS_ERR_EMPTY";
    case TESS_ERR_NOT_FOUND: return "TESS_ERR_NOT_FOUND";
    case TESS_ERR_IO:        return "TESS_ERR_IO";
    case TESS_ERR_RANGE:     return "TESS_ERR_RANGE";
    }
    return "TESS_ERR_?";
}

tess_rect tess_rect_inset(tess_rect r, int32_t dx, int32_t dy)
{
    const int32_t cx = r.x0 + tess_rect_width(r) / 2;
    const int32_t cy = r.y0 + tess_rect_height(r) / 2;

    r.x0 += dx;
    r.x1 -= dx;
    r.y0 += dy;
    r.y1 -= dy;

    /* An inset larger than half the extent would invert the rectangle, and an
     * inverted rectangle silently breaks every containment test downstream --
     * it answers "outside" for every point, which reads like a marker that has
     * gone off-screen rather than like a bad argument. Collapse to the centre
     * instead. */
    if (r.x1 < r.x0)
    {
        r.x0 = cx;
        r.x1 = cx;
    }
    if (r.y1 < r.y0)
    {
        r.y0 = cy;
        r.y1 = cy;
    }
    return r;
}

bool tess_tile_is_valid(tess_tile tile)
{
    if (!tess_zoom_is_valid((int) tile.zoom))
    {
        return false;
    }

    const int32_t n = tess_tiles_per_axis((int) tile.zoom);
    return tile.x >= 0 && tile.x < n && tile.y >= 0 && tile.y < n;
}

/* Append a decimal integer, returning false if it would not fit.
 *
 * Written out rather than reached for via snprintf per token because the
 * bounds check has to be per-append: a path is built from several pieces and
 * the only useful answer to "the fourth piece did not fit" is to stop. */
static bool append_int(char *buf, size_t buf_size, size_t *len, int32_t value)
{
    char digits[12];
    const int written = snprintf(digits, sizeof digits, "%ld", (long) value);

    if (written < 0 || (size_t) written >= sizeof digits)
    {
        return false;
    }
    if (*len + (size_t) written + 1u > buf_size)
    {
        return false;
    }

    memcpy(&buf[*len], digits, (size_t) written);
    *len += (size_t) written;
    return true;
}

static bool append_char(char *buf, size_t buf_size, size_t *len, char c)
{
    if (*len + 2u > buf_size)
    {
        return false;
    }
    buf[(*len)++] = c;
    return true;
}

tess_status tess_tile_path(char *buf, size_t buf_size, const char *pattern, tess_tile tile)
{
    if (buf == NULL || buf_size == 0u)
    {
        return TESS_ERR_ARG;
    }

    /* Fail closed. A caller that ignores the status and uses the buffer anyway
     * gets an empty string rather than whatever was on the stack. */
    buf[0] = '\0';

    if (!tess_tile_is_valid(tile))
    {
        return TESS_ERR_ARG;
    }

    if (pattern == NULL)
    {
        pattern = TESS_TILE_PATH_DEFAULT;
    }

    size_t len = 0;

    for (const char *p = pattern; *p != '\0'; p++)
    {
        /* A token is exactly "{z}", "{x}" or "{y}". Anything else beginning
         * with '{' is copied through, so a pattern containing a literal brace
         * is not silently mangled. */
        if (p[0] == '{' && p[1] != '\0' && p[2] == '}')
        {
            int32_t value = 0;
            bool is_token = true;

            switch (p[1])
            {
            case 'z': value = tile.zoom; break;
            case 'x': value = tile.x; break;
            case 'y': value = tile.y; break;
            default:  is_token = false; break;
            }

            if (is_token)
            {
                if (!append_int(buf, buf_size, &len, value))
                {
                    buf[0] = '\0';
                    return TESS_ERR_RANGE;
                }
                p += 2;
                continue;
            }
        }

        if (!append_char(buf, buf_size, &len, *p))
        {
            buf[0] = '\0';
            return TESS_ERR_RANGE;
        }
    }

    buf[len] = '\0';
    return TESS_OK;
}
