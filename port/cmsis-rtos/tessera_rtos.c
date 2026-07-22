/* SPDX-License-Identifier: MIT */

#include "tessera_rtos.h"

#include "cmsis_os.h"

static void rtos_acquire(void *ctx)
{
    tess_rtos_loader *loader = (tess_rtos_loader *) ctx;

    /* osWaitForever, not a timeout with a fallback. A timeout here would mean
     * carrying on without the lock, which is worse than blocking: the
     * critical sections are a handful of pointer comparisons and cannot be
     * held for long by anything except a bug. */
    if (loader != NULL && loader->mutex != NULL)
    {
        (void) osMutexWait((osMutexId) loader->mutex, osWaitForever);
    }
}

static void rtos_release(void *ctx)
{
    tess_rtos_loader *loader = (tess_rtos_loader *) ctx;

    if (loader != NULL && loader->mutex != NULL)
    {
        (void) osMutexRelease((osMutexId) loader->mutex);
    }
}

osMutexDef(tess_map_mutex);

tess_status tess_rtos_loader_init(tess_rtos_loader *loader, uint32_t idle_ms)
{
    if (loader == NULL)
    {
        return TESS_ERR_ARG;
    }

    loader->map = NULL;
    loader->running = false;
    loader->idle_ms = (idle_ms > 0u) ? idle_ms : 20u;

    loader->mutex = (void *) osMutexCreate(osMutex(tess_map_mutex));

    /* Propagated, not discarded. A NULL mutex does not announce itself: every
     * acquire and release quietly does nothing, and the code around them still
     * reads as if it were serialised. */
    return (loader->mutex != NULL) ? TESS_OK : TESS_ERR_IO;
}

tess_lock tess_rtos_lock(tess_rtos_loader *loader)
{
    tess_lock lock;
    lock.ctx = loader;
    lock.acquire = rtos_acquire;
    lock.release = rtos_release;
    return lock;
}

/*
 * `running` is written by whichever thread calls stop() and read by the loader,
 * so it goes through the mutex like everything else shared here.
 *
 * `volatile bool` is the reflex, and ThreadSanitizer calls it a data race --
 * correctly, for the reason given under tess_lock in <tessera/port.h>. On this
 * architecture it would very likely have worked anyway, which is exactly the
 * property that lets a threading bug survive testing.
 *
 * The cost is one uncontended mutex per iteration, next to a read from the
 * medium.
 */
static void set_running(tess_rtos_loader *loader, bool value)
{
    rtos_acquire(loader);
    loader->running = value;
    rtos_release(loader);
}

static bool get_running(tess_rtos_loader *loader)
{
    rtos_acquire(loader);
    const bool value = loader->running;
    rtos_release(loader);
    return value;
}

void tess_rtos_loader_run(tess_rtos_loader *loader)
{
    if (loader == NULL || loader->map == NULL)
    {
        return;
    }

    set_running(loader, true);

    while (get_running(loader))
    {
        /* No lock is held across this. tess_map_service brackets its own
         * shared-state access and holds nothing across the SD read, which is
         * the whole reason this loop can be five lines. */
        if (tess_map_service(loader->map) == TESS_ERR_EMPTY)
        {
            osDelay(loader->idle_ms);
        }
    }
}

void tess_rtos_loader_stop(tess_rtos_loader *loader)
{
    if (loader != NULL)
    {
        set_running(loader, false);
    }
}

void tess_rtos_loader_deinit(tess_rtos_loader *loader)
{
    if (loader == NULL || loader->mutex == NULL)
    {
        return;
    }

    /* Not through set_running: the mutex is about to go, and the caller has
     * already joined the loader thread, so there is nobody left to race. */
    loader->running = false;
    (void) osMutexDelete((osMutexId) loader->mutex);

    /* Cleared, so that a lock vtable still held by a map becomes a no-op
     * rather than a use-after-free. Not a licence to keep using the map --
     * without the mutex it is no longer thread-safe -- but a torn frame beats
     * a fault while a screen is being torn down. */
    loader->mutex = NULL;
    loader->map = NULL;
}
