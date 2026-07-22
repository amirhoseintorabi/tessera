/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_PORT_CMSIS_RTOS_H
#define TESSERA_PORT_CMSIS_RTOS_H

/*
 * The mutex and the loader loop, on CMSIS-RTOS.
 *
 * Both are short, and both are short on purpose: the engine does its own
 * locking internally and releases on every path, so the loop below holds no
 * lock at all and the mutex is a straight pass-through to the RTOS.
 *
 * Why the loop is allowed to be this short: a loader that releases its lock
 * only on the path where the work succeeded stops the map dead the first time
 * the queue empties between the check and the fetch, which two threads make
 * routine. Having no lock in the loop at all removes that failure mode rather
 * than getting it right.
 *
 * On using the RTOS mutex rather than a flag, see tess_lock in
 * <tessera/port.h>.
 */

#include "tessera/map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void *mutex;      /* osMutexId; void so this header needs no RTOS include */
    tess_map *map;

    /* Guarded by `mutex`; see the note above set_running() in tessera_rtos.c
     * for why this is not simply volatile. */
    bool running;

    /* How long to sleep when there is nothing to load. Long enough not to
     * spin, short enough that a pan feels immediate. */
    uint32_t idle_ms;
} tess_rtos_loader;

/*
 * Create the mutex. Must be called before tess_map_init, since the map keeps
 * the lock vtable that tess_rtos_lock returns.
 *
 * Returns TESS_ERR_IO if the RTOS would not give a mutex. Worth propagating
 * rather than ignoring: an unchecked creation failure leaves a NULL handle,
 * and every critical section guarded by it silently becomes a no-op that still
 * looks like locked code.
 */
tess_status tess_rtos_loader_init(tess_rtos_loader *loader, uint32_t idle_ms);

/* The lock vtable to put in tess_map_config. */
tess_lock tess_rtos_lock(tess_rtos_loader *loader);

/*
 * The loader body. Runs until tess_rtos_loader_stop is called, servicing one
 * tile per iteration and sleeping only when there is nothing to do.
 *
 * Intended as the whole of a thread function:
 *
 *     static void loader_thread(void const *arg) {
 *         tess_rtos_loader_run((tess_rtos_loader *) arg);
 *     }
 */
void tess_rtos_loader_run(tess_rtos_loader *loader);

/* Ask the loop to finish after its current tile. */
void tess_rtos_loader_stop(tess_rtos_loader *loader);

/*
 * Release the mutex.
 *
 * Must not be called until the loader thread has actually stopped -- stop()
 * only asks it to, and the thread may still be inside a critical section.
 * Join the thread first.
 *
 * On a unit where the map is created at boot and lives until power-off this
 * never runs, which is exactly why it is easy to leave out. It exists because
 * a map screen that can be opened and closed would otherwise leak one RTOS
 * mutex per visit, and an RTOS's mutex pool is small and fixed.
 */
void tess_rtos_loader_deinit(tess_rtos_loader *loader);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PORT_CMSIS_RTOS_H */
