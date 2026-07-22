/* SPDX-License-Identifier: MIT */

#include "tessera_fatfs.h"

#include "ff.h"

/*
 * One FIL on the stack per call, rather than one shared static.
 *
 * A file-scope FIL works as long as there is exactly one thread reading, and
 * fails silently the moment that stops being true -- a second loader, or an
 * application that services the map from its own thread to fill the first
 * screen, corrupts an in-flight read with no diagnostic. A FIL is around
 * 550 bytes with FF_FS_TINY off, which a loader thread's stack can carry.
 */

static tess_status fatfs_load(void *ctx, tess_tile tile, void *image)
{
    tess_fatfs_tiles *state = (tess_fatfs_tiles *) ctx;

    if (state == NULL || image == NULL || state->image_capacity == 0u)
    {
        return TESS_ERR_ARG;
    }

    /* Bounded, and refuses addresses that cannot exist. */
    char path[TESS_TILE_PATH_MAX];
    const tess_status named = tess_tile_path(path, sizeof path, state->pattern, tile);
    if (named != TESS_OK)
    {
        state->io_errors++;
        return named;
    }

    FILINFO info;
    if (f_stat(path, &info) != FR_OK)
    {
        /* No such tile is normal near the edge of the mapped area, so it is
         * TESS_ERR_NOT_FOUND rather than an error -- the engine draws a
         * placeholder and does not retry until the view changes. */
        state->not_found++;
        return TESS_ERR_NOT_FOUND;
    }

    const size_t size = (size_t) info.fsize;

    /* The length comes from the card, so it is checked against the
     * destination *before* anything is opened, let alone read. */
    if (size > state->image_capacity)
    {
        state->too_large++;
        return TESS_ERR_RANGE;
    }
    if (size < state->minimum_size || size == 0u)
    {
        state->too_large++;
        return TESS_ERR_RANGE;
    }

    FIL file;
    if (f_open(&file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK)
    {
        /* stat succeeded and open did not: the card was removed between the
         * two, or the filesystem is damaged. Either way it is an error rather
         * than an absence. */
        state->io_errors++;
        return TESS_ERR_IO;
    }

    UINT read = 0;
    const FRESULT result = f_read(&file, image, (UINT) size, &read);

    /* Exactly one exit after the open, and it closes. */
    f_close(&file);

    /* Both the read's own result and the byte count. FatFs reports a short
     * read as FR_OK with a smaller count, so the FRESULT alone does not
     * distinguish a whole file from half of one. */
    if (result != FR_OK)
    {
        state->io_errors++;
        return TESS_ERR_IO;
    }
    if ((size_t) read != size)
    {
        state->short_reads++;
        return TESS_ERR_IO;
    }

    state->reads_ok++;
    return TESS_OK;
}

tess_tile_source tess_fatfs_tile_source(tess_fatfs_tiles *state)
{
    tess_tile_source source;
    source.ctx = state;

    /* A source with no capacity has no safe way to bound a read, so it is
     * handed back inert. The engine treats a NULL load as "no tiles
     * available" and draws placeholders, which is a visible, harmless failure
     * rather than an unbounded write. */
    source.load = (state != NULL && state->image_capacity > 0u) ? fatfs_load : NULL;
    return source;
}
