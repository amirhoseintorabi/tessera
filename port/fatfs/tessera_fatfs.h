/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PORT_FATFS_H
#define TESSERA_PORT_FATFS_H

/*
 * A tile source backed by a FatFs volume -- an SD card, eMMC, or anything else
 * FatFs mounts.
 *
 * The whole of it is one short function, and every check in it exists because
 * removable storage is untrusted input. Its contents are chosen by whoever
 * last held the card, not by whoever wrote the firmware, and the three ways a
 * tile read goes wrong are all forms of believing it:
 *
 *   The path. Built from a tile address, so it has to be bounded and the
 *   address has to be one that can exist. tess_tile_path does both; a
 *   fixed-size buffer and an unbounded formatter is the version that works
 *   until an out-of-range address reaches it.
 *
 *   The length. It comes from the file, and the destination is a fixed buffer,
 *   so the two must be compared -- before opening anything, not after reading.
 *   An oversized file otherwise overruns the tile buffer into whatever the
 *   linker placed after it.
 *
 *   The result. f_read reports a short read as FR_OK with a smaller count, so
 *   checking the FRESULT alone is not enough: the byte count has to be checked
 *   too, or a truncated file is handed to the image decoder as if it were
 *   whole.
 *
 * There is exactly one exit between the open and the close, which is what
 * keeps the FIL from leaking. FatFs file objects are a fixed resource, and the
 * conditions that make a read fail -- a missing tile, a full cache upstream --
 * are ordinary rather than rare.
 */

#include "tessera/port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /*
     * Path pattern, in tess_tile_path's {z}/{x}/{y} form. NULL selects
     * TESS_TILE_PATH_DEFAULT. Not copied -- it must outlive the source.
     */
    const char *pattern;

    /*
     * Bytes available in each cache slot's image buffer.
     *
     * This is the number the size check is made against, so it must be the
     * real size of the smallest buffer any slot points at. A value larger than
     * that turns the check into a lie, which is worse than not having one.
     */
    size_t image_capacity;

    /*
     * Smallest file this will accept, guarding against a truncated or
     * zero-length tile reaching an image decoder. 0 disables the check.
     */
    size_t minimum_size;

    /* Counters, for a diagnostics screen. */
    uint32_t reads_ok;
    uint32_t not_found;
    uint32_t too_large;     /* the file would not fit the slot */
    uint32_t short_reads;   /* fewer bytes arrived than the file claims */
    uint32_t io_errors;
} tess_fatfs_tiles;

/*
 * Bind a tile source to `state`.
 *
 * The returned vtable holds a pointer to `state`, so `state` must outlive it.
 * Returns a source with a NULL `load` if the configuration is unusable -- a
 * zero capacity, most obviously -- so that a caller that ignores the check
 * gets a map that draws placeholders rather than one that writes into a
 * buffer whose size nobody knows.
 */
tess_tile_source tess_fatfs_tile_source(tess_fatfs_tiles *state);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PORT_FATFS_H */
