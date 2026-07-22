/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_INTEROP_FF_H
#define TESSERA_INTEROP_FF_H

/*
 * Interoperability declarations for FatFs.
 *
 * NOT the real ff.h and not derived from it: hand-written declarations of the
 * six calls ../tessera_fatfs.c uses, taken from the published API, so that the
 * port can be built and exercised on a workstation.
 *
 * FatFs itself is ChaN's, under its own permissive licence, and none of it is
 * here. Put the genuine headers on the include path ahead of this directory to
 * build for a target; the port includes "ff.h" by plain name.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t FSIZE_t;
typedef char TCHAR;

typedef enum
{
    FR_OK = 0,
    FR_DISK_ERR,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_DENIED,
    FR_INVALID_OBJECT,
    FR_NOT_ENOUGH_CORE
} FRESULT;

#define FA_READ           0x01
#define FA_OPEN_EXISTING  0x00

typedef struct
{
    FSIZE_t fsize;
    BYTE fattrib;
    TCHAR fname[256];
} FILINFO;

/* The real FIL is a good deal larger and holds the cluster chain state. Only
 * its existence and its being a value type matter to the port. */
typedef struct
{
    void *handle;
    FSIZE_t size;
} FIL;

FRESULT f_stat(const TCHAR *path, FILINFO *info);
FRESULT f_open(FIL *file, const TCHAR *path, BYTE mode);
FRESULT f_read(FIL *file, void *buffer, UINT to_read, UINT *read);
FRESULT f_close(FIL *file);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_INTEROP_FF_H */
