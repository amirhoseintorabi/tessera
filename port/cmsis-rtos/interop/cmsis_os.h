/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_INTEROP_CMSIS_OS_H
#define TESSERA_INTEROP_CMSIS_OS_H

/*
 * Interoperability declarations for CMSIS-RTOS v1.
 *
 * NOT the real cmsis_os.h and not derived from it: hand-written declarations
 * of the five calls ../tessera_rtos.c uses, so that the port can be built and
 * run on a workstation over pthreads -- which is what makes the loader's
 * locking testable under ThreadSanitizer.
 *
 * Put the genuine CMSIS headers on the include path ahead of this directory to
 * build for a target; the port includes "cmsis_os.h" by plain name.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct os_mutex_def
{
    const char *name;
} osMutexDef_t;

typedef void *osMutexId;

typedef enum
{
    osOK = 0,
    osErrorParameter = 0x80,
    osErrorResource = 0x81,
    osErrorISR = 0x82
} osStatus;

#define osWaitForever 0xFFFFFFFFu

#define osMutexDef(name) static const osMutexDef_t os_mutex_def_##name = {#name}
#define osMutex(name)    (&os_mutex_def_##name)

osMutexId osMutexCreate(const osMutexDef_t *definition);
osStatus osMutexWait(osMutexId mutex, uint32_t timeout_ms);
osStatus osMutexRelease(osMutexId mutex);
osStatus osMutexDelete(osMutexId mutex);
osStatus osDelay(uint32_t milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_INTEROP_CMSIS_OS_H */
