/* SPDX-License-Identifier: MIT */

/*
 * The FatFs interoperability declarations, implemented over stdio.
 *
 * Enough of FatFs's observable behaviour for the port to be run rather than
 * merely compiled: a short read really does report fewer bytes, a missing file
 * really does come back FR_NO_FILE, and f_stat really does report the size the
 * bounds check is made against. That lets the tests drive an oversized tile, a
 * truncated one and a missing one against the same source file that gets
 * compiled for the target.
 */

#include "ff.h"

#include <stdio.h>
#include <string.h>

FRESULT f_stat(const TCHAR *path, FILINFO *info)
{
    if (path == NULL || info == NULL)
    {
        return FR_INVALID_OBJECT;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return FR_NO_FILE;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return FR_DISK_ERR;
    }

    const long size = ftell(f);
    fclose(f);

    if (size < 0)
    {
        return FR_DISK_ERR;
    }

    memset(info, 0, sizeof(*info));
    info->fsize = (FSIZE_t) size;
    snprintf(info->fname, sizeof(info->fname), "%s", path);
    return FR_OK;
}

FRESULT f_open(FIL *file, const TCHAR *path, BYTE mode)
{
    if (file == NULL || path == NULL)
    {
        return FR_INVALID_OBJECT;
    }
    if ((mode & FA_READ) == 0)
    {
        return FR_DENIED;  /* the port only ever reads */
    }

    memset(file, 0, sizeof(*file));
    file->handle = fopen(path, "rb");
    return (file->handle != NULL) ? FR_OK : FR_NO_FILE;
}

FRESULT f_read(FIL *file, void *buffer, UINT to_read, UINT *read)
{
    if (file == NULL || file->handle == NULL || buffer == NULL || read == NULL)
    {
        return FR_INVALID_OBJECT;
    }

    const size_t got = fread(buffer, 1, (size_t) to_read, (FILE *) file->handle);
    *read = (UINT) got;

    /* FatFs reports a short read as FR_OK with a smaller count. Reproduce that
     * rather than the more convenient behaviour of failing -- the whole point
     * of the port's byte-count check is this case. */
    if (got < (size_t) to_read && ferror((FILE *) file->handle))
    {
        return FR_DISK_ERR;
    }
    return FR_OK;
}

FRESULT f_close(FIL *file)
{
    if (file == NULL)
    {
        return FR_INVALID_OBJECT;
    }
    if (file->handle != NULL)
    {
        fclose((FILE *) file->handle);
        file->handle = NULL;
    }
    return FR_OK;
}
