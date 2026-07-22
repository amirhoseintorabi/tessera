/* SPDX-License-Identifier: MIT */

/*
 * The CMSIS-RTOS interoperability declarations, implemented over pthreads.
 *
 * Real mutexes and a real sleep, so the tests can run the loader loop on one
 * thread while another pans the map and have ThreadSanitizer's findings mean
 * something. A stub that did nothing would compile the port without exercising
 * the one thing the port exists to get right.
 */

#include "cmsis_os.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

osMutexId osMutexCreate(const osMutexDef_t *definition)
{
    (void) definition;

    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (mutex == NULL)
    {
        return NULL;
    }
    if (pthread_mutex_init(mutex, NULL) != 0)
    {
        free(mutex);
        return NULL;
    }
    return (osMutexId) mutex;
}

osStatus osMutexWait(osMutexId mutex, uint32_t timeout_ms)
{
    (void) timeout_ms;  /* the port only ever waits forever */

    if (mutex == NULL)
    {
        return osErrorParameter;
    }
    return (pthread_mutex_lock((pthread_mutex_t *) mutex) == 0) ? osOK : osErrorResource;
}

osStatus osMutexRelease(osMutexId mutex)
{
    if (mutex == NULL)
    {
        return osErrorParameter;
    }
    return (pthread_mutex_unlock((pthread_mutex_t *) mutex) == 0) ? osOK : osErrorResource;
}

osStatus osMutexDelete(osMutexId mutex)
{
    if (mutex == NULL)
    {
        return osErrorParameter;
    }

    pthread_mutex_destroy((pthread_mutex_t *) mutex);
    free(mutex);
    return osOK;
}

osStatus osDelay(uint32_t milliseconds)
{
    struct timespec request;
    request.tv_sec = (time_t)(milliseconds / 1000u);
    request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;

    while (nanosleep(&request, &request) != 0)
    {
        /* Interrupted by a signal: nanosleep has written the remaining time
         * back into `request`, so going round again finishes the wait rather
         * than returning early. */
    }
    return osOK;
}
