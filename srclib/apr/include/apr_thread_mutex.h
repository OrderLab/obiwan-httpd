/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef APR_THREAD_MUTEX_H
#define APR_THREAD_MUTEX_H

/**
 * @file apr_thread_mutex.h
 * @brief APR Thread Mutex Routines
 */

#include "apr.h"
#include "apr_errno.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#if APR_HAS_THREADS || defined(DOXYGEN)

/**
 * @defgroup apr_thread_mutex Thread Mutex Routines
 * @ingroup APR 
 * @{
 */

/** Opaque thread-local mutex structure */
typedef struct apr_thread_mutex_t apr_thread_mutex_t;

#define APR_THREAD_MUTEX_DEFAULT  0x0   /**< platform-optimal lock behavior */
#define APR_THREAD_MUTEX_NESTED   0x1   /**< enable nested (recursive) locks */
#define APR_THREAD_MUTEX_UNNESTED 0x2   /**< disable nested locks */
#define APR_THREAD_MUTEX_TIMED    0x4   /**< enable timed locks */

/* Delayed the include to avoid a circular reference */
#include "apr_pools.h"
#include "apr_time.h"

#define HAS_ORBIT 1

/**
 * Create and initialize a mutex that can be used to synchronize threads.
 * @param mutex the memory address where the newly created mutex will be
 *        stored.
 * @param flags Or'ed value of:
 * <PRE>
 *           APR_THREAD_MUTEX_DEFAULT   platform-optimal lock behavior.
 *           APR_THREAD_MUTEX_NESTED    enable nested (recursive) locks.
 *           APR_THREAD_MUTEX_UNNESTED  disable nested locks (non-recursive).
 * </PRE>
 * @param pool the pool from which to allocate the mutex.
 * @warning Be cautious in using APR_THREAD_MUTEX_DEFAULT.  While this is the
 * most optimal mutex based on a given platform's performance characteristics,
 * it will behave as either a nested or an unnested lock.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_create(apr_thread_mutex_t **mutex,
                                                  unsigned int flags,
                                                  apr_pool_t *pool);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_create(apr_thread_mutex_t **mutex,
        unsigned int flags, apr_pool_t *pool,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t) apr_thread_mutex_create(
        apr_thread_mutex_t **mutex, unsigned int flags, apr_pool_t *pool)
{
    return __apr_thread_mutex_create(mutex, flags, pool, "unknown", "unknown", 0);
}
#define apr_thread_mutex_create(mutex, flags, pool) \
    __apr_thread_mutex_create(mutex, flags, pool, __func__, __FILE__, __LINE__)
#endif
/**
 * Acquire the lock for the given mutex. If the mutex is already locked,
 * the current thread will be put to sleep until the lock becomes available.
 * @param mutex the mutex on which to acquire the lock.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_lock(apr_thread_mutex_t *mutex);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_lock(apr_thread_mutex_t *mutex,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t)
apr_thread_mutex_lock(apr_thread_mutex_t *mutex)
{
    return __apr_thread_mutex_lock(mutex, "unknown", "unknown", 0);
}
#define apr_thread_mutex_lock(mutex) \
    __apr_thread_mutex_lock(mutex, __func__, __FILE__, __LINE__)
#endif

/**
 * Attempt to acquire the lock for the given mutex. If the mutex has already
 * been acquired, the call returns immediately with APR_EBUSY. Note: it
 * is important that the APR_STATUS_IS_EBUSY(s) macro be used to determine
 * if the return value was APR_EBUSY, for portability reasons.
 * @param mutex the mutex on which to attempt the lock acquiring.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_trylock(apr_thread_mutex_t *mutex);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_trylock(apr_thread_mutex_t *mutex,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t)
apr_thread_mutex_trylock(apr_thread_mutex_t *mutex)
{
    return __apr_thread_mutex_trylock(mutex, "unknown", "unknown", 0);
}
#define apr_thread_mutex_trylock(mutex) \
    __apr_thread_mutex_trylock(mutex, __func__, __FILE__, __LINE__)
#endif

/**
 * Attempt to acquire the lock for the given mutex until timeout expires.
 * If the acquisition time outs, the call returns with APR_TIMEUP.
 * @param mutex the mutex on which to attempt the lock acquiring.
 * @param timeout the relative timeout (microseconds).
 * @note A timeout negative or nul means immediate attempt, returning
 *       APR_TIMEUP without blocking if it the lock is already acquired.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_timedlock(apr_thread_mutex_t *mutex,
                                                 apr_interval_time_t timeout);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_timedlock(apr_thread_mutex_t *mutex,
        apr_interval_time_t timeout,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t)
apr_thread_mutex_timedlock(apr_thread_mutex_t *mutex, apr_interval_time_t timeout)
{
    return __apr_thread_mutex_timedlock(mutex, timeout, "unknown", "unknown", 0);
}
#define apr_thread_mutex_timedlock(mutex, timeout) \
    __apr_thread_mutex_timedlock(mutex, timeout, __func__, __FILE__, __LINE__)
#endif

/**
 * Release the lock for the given mutex.
 * @param mutex the mutex from which to release the lock.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_unlock(apr_thread_mutex_t *mutex);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_unlock(apr_thread_mutex_t *mutex,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t)
apr_thread_mutex_unlock(apr_thread_mutex_t *mutex)
{
    return __apr_thread_mutex_unlock(mutex, "unknown", "unknown", 0);
}
#define apr_thread_mutex_unlock(mutex) \
    __apr_thread_mutex_unlock(mutex, __func__, __FILE__, __LINE__)
#endif

/**
 * Destroy the mutex and free the memory associated with the lock.
 * @param mutex the mutex to destroy.
 */
#if !HAS_ORBIT || defined(DOXYGEN)
APR_DECLARE(apr_status_t) apr_thread_mutex_destroy(apr_thread_mutex_t *mutex);
#else
APR_DECLARE(apr_status_t) __apr_thread_mutex_destroy(apr_thread_mutex_t *mutex,
        const char *func, const char *file, int line);
static inline APR_DECLARE(apr_status_t)
apr_thread_mutex_destroy(apr_thread_mutex_t *mutex)
{
    return __apr_thread_mutex_destroy(mutex, "unknown", "unknown", 0);
}
#define apr_thread_mutex_destroy(mutex) \
    __apr_thread_mutex_destroy(mutex, __func__, __FILE__, __LINE__)
#endif

/**
 * Get the pool used by this thread_mutex.
 * @return apr_pool_t the pool
 */
APR_POOL_DECLARE_ACCESSOR(thread_mutex);

#undef HAS_ORBIT

#endif /* APR_HAS_THREADS */

/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* ! APR_THREAD_MUTEX_H */
