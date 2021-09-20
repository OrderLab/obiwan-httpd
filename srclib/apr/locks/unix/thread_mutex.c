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

#include "apr_arch_thread_mutex.h"
#define APR_WANT_MEMFUNC
#include "apr_want.h"

#if APR_HAS_THREADS

#include "orbit.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <assert.h>

// FIXME: using a fixed size slots and expensive traverse just for PoC
#define NSLOTS 1024
#define OBWDG_INTERVAL 1
#define OBWDG_TIMEOUT 1

/* Watchdog counter that is reset by the orbit and inc by the main program. */
static struct ob_watchdog_counter_slot {
    /* Generation number.
     * Each time this slot is reused, the slot will be incremented twice:
     * odd for grabbing the slot, even for releasing it. This value is allowed
     * to safely overflow. */
    /* uint16_t gen; */
    /* Counter for current generation */
    uint16_t counter;
    /* Number of held locks */
    uint16_t nholds;
} *obwdg_counter_slots = NULL;
static size_t counter_slot_used = 0;

/* Watchdog debug information that can be used for full diagnosis */
static struct ob_mutex_watchdog_info {
    /* For diagnosis, we make an assumption that a lock will always be acquired
     * and released by the same thread. */
    pid_t holder_tid;
    time_t time;
    size_t counter_slot_idx;
    apr_thread_mutex_t *mutex;
} *obwdg_info_slots = NULL;
static size_t info_slot_used = 0;

#define INVIDX ((size_t)-1)
static __thread size_t counter_slot_idx = INVIDX;

static struct orbit_pool *obwdg_counter_pool = NULL;
static struct orbit_pool *obwdg_info_pool = NULL;
static struct orbit_pool *obwdg_scratch_pool = NULL;
static struct orbit_allocator *obwdg_counter_alloc = NULL;
static struct orbit_allocator *obwdg_info_alloc = NULL;

static struct orbit_module *obwdg = NULL;
static pthread_t obwdg_bg;

static unsigned long obwdg_entry(void *store, void *argbuf);
static void *obwdg_bg_loop(void *aux);
static unsigned long obwdg_diagnosis(void *store, void *argbuf);
static void *obwdg_create_store(void);

void obwdg_init() {
    if (obwdg_info_alloc != NULL) return;
    obwdg_counter_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_info_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_scratch_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_counter_alloc = orbit_allocator_from_pool(obwdg_counter_pool, true);
    obwdg_info_alloc = orbit_allocator_from_pool(obwdg_info_pool, true);
    orbit_scratch_set_pool(obwdg_scratch_pool);

    obwdg_counter_slots = orbit_calloc(obwdg_counter_alloc,
            sizeof(struct ob_watchdog_counter_slot) * NSLOTS);
    obwdg_info_slots = orbit_calloc(obwdg_info_alloc,
            sizeof(struct ob_mutex_watchdog_info) * NSLOTS);

    obwdg = orbit_create("ob mutex watchdog", obwdg_entry, NULL);
    int ret = pthread_create(&obwdg_bg, NULL, obwdg_bg_loop, NULL);
    assert(ret == 0);
}

#define likely(x) __builtin_expect((x), 1)

static inline void grab_counter_slot() {
    if (likely(counter_slot_idx != INVIDX)) return;

    /* for (int i = 0; i < NSLOTS; ++i) {
        uint16_t gen = __atomic_load_n(&obwdg_counter_slots[i].gen, __ATOMIC_ACQUIRE);
        if ((gen & 1) == 0) {
            if (!__atomic_compare_exchange_n(&obwdg_counter_slots[i].gen,
                        &gen, gen+1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELEASE))
                continue;
            obwdg_counter_slots[i].counter = 0;
            counter_slot_idx = i;
            return;
        }
    } */
    counter_slot_idx = __atomic_fetch_add(&counter_slot_used, 1, __ATOMIC_ACQUIRE);
    if (counter_slot_idx >= NSLOTS) {
        fprintf(stderr, "orbit: counter slots used up\n");
        abort();
    }
}

static void release_counter_slot() {
    /* __atomic_add_fetch(&obwdg_counter_slots[counter_slot_idx].gen, 1, __ATOMIC_RELEASE); */
    counter_slot_idx = INVIDX;
}

static inline struct ob_mutex_watchdog_info *grab_info_slot() {
    size_t slot_idx = __atomic_fetch_add(&info_slot_used, 1, __ATOMIC_ACQUIRE);
    if (slot_idx >= NSLOTS) {
        fprintf(stderr, "orbit: info slots used up\n");
        abort();
    }
    return obwdg_info_slots + slot_idx;
}

static inline void release_info_slot(struct ob_mutex_watchdog_info *info) {
    (void)info;
}

static inline void obwdg_acquired(apr_thread_mutex_t *mutex) {
    grab_counter_slot();

    size_t idx = counter_slot_idx;
    ++obwdg_counter_slots[idx].counter;
    ++obwdg_counter_slots[idx].nholds;

    /* Older Linux does not have gittid wrapper */
    /* mutex->obwdginfo->holder_tid = gettid(); */
    mutex->obwdginfo->holder_tid = syscall(SYS_gettid);
    mutex->obwdginfo->time = time(NULL);
    mutex->obwdginfo->counter_slot_idx = idx;
    mutex->obwdginfo->mutex = mutex;
}

static inline void obwdg_released(apr_thread_mutex_t *mutex) {
    size_t idx = counter_slot_idx;
    ++obwdg_counter_slots[idx].counter;
    --obwdg_counter_slots[idx].nholds;

    mutex->obwdginfo->holder_tid = 0;
    mutex->obwdginfo->time = time(NULL);
    mutex->obwdginfo->counter_slot_idx = INVIDX;
    mutex->obwdginfo->mutex = NULL;
}

struct obwdg_store {
    struct slot_stats {
        uint16_t nholds;
        int unchanged_cnt;
    } *last_values;
};

static void *obwdg_create_store(void) {
    struct obwdg_store *store = (struct obwdg_store*)malloc(sizeof(struct obwdg_store));

    store->last_values = (struct slot_stats*)calloc(NSLOTS, sizeof(struct slot_stats));

    return store;
}

struct obwdg_entry_args {
    struct ob_watchdog_counter_slot *counter_slots;
    size_t counter_slot_used;
};

struct obwdg_diagnosis_args {
    struct ob_mutex_watchdog_info *info_slots;
    size_t counter_slot_used;
    size_t info_slot_used;
};

static unsigned long obwdg_helper_reset(size_t nargs, unsigned long argv[]) {
    size_t nslots = argv[0];
    // TODO: this could have the race condition that counter is modified
    // during the check. A better way is -= the old counter.
    for (size_t i = 0; i < nslots; ++i)
        obwdg_counter_slots[i].counter = 0;
    return 0;
}

#define xstringify(x) stringify(x)
#define stringify(x) #x

static unsigned long obwdg_helper_print(size_t nargs, unsigned long argv[]) {
    size_t slot_idx = argv[0];
    struct ob_mutex_watchdog_info *slot = obwdg_info_slots + slot_idx;
    fprintf(stderr, "orbit: slot %lu has been occupied for "xstringify(OBWDG_TIMEOUT)" seconds\n", slot_idx);
    fprintf(stderr, "orbit: slot %lu found one holder at unix time %ld, tid=%d, mutex=%p\n",
            slot_idx, slot->time, slot->holder_tid, slot->mutex);
    return 0;
}

static unsigned long obwdg_entry(void *store_, void *argbuf) {
    struct obwdg_store *store = (struct obwdg_store*)store_;
    struct obwdg_entry_args *args = (struct obwdg_entry_args*)argbuf;
    int next_check = 0;
    int ret;

    struct orbit_scratch scratch;
    ret = orbit_scratch_create(&scratch);
    assert(ret == 0);

    for (int i = 0; i < args->counter_slot_used; ++i) {
        struct slot_stats *last_slot = store->last_values + i;
        struct ob_watchdog_counter_slot *curr_slot = args->counter_slots + i;

        if (curr_slot->nholds != 0 && curr_slot->nholds == last_slot->nholds
                && curr_slot->counter == 0)
        {
            /* The counter hasn't been changed for 1 sec. Note down this in
             * unchanged_cnt. */
            if (++last_slot->unchanged_cnt % OBWDG_TIMEOUT == 0) {
                /* We should trigger a diagnosis held for 1 min. */
                next_check = 1;
                fprintf(stderr, "orbit: found slot %d to have long running thread\n", i);
            }
        } else {
            /* Either nholds or counter has been changed, reset the slot. */
            last_slot->nholds = curr_slot->nholds;
            last_slot->unchanged_cnt = 0;
        }
    }

    /* Clear value for remaining slots */
    size_t used_bytes = args->counter_slot_used * sizeof(struct slot_stats);
    size_t total_bytes = NSLOTS * sizeof(struct slot_stats);
    if (args->counter_slot_used < NSLOTS)
        memset(store->last_values + used_bytes, 0, NSLOTS - used_bytes);

    unsigned long argv[] = { args->counter_slot_used, };
    orbit_scratch_push_operation(&scratch, obwdg_helper_reset, 1, argv);

    ret = orbit_sendv(&scratch);
    assert(ret == 0);

    return next_check;
}

static unsigned long obwdg_diagnosis(void *store_, void *argbuf) {
    struct obwdg_store *store = (struct obwdg_store*)store_;
    struct obwdg_diagnosis_args *args = (struct obwdg_diagnosis_args*)argbuf;
    int ret;

    struct orbit_scratch scratch;
    ret = orbit_scratch_create(&scratch);
    assert(ret == 0);

    for (size_t i = 0; i < args->counter_slot_used; ++i) {
        struct slot_stats *last_slot = store->last_values + i;

        /* This is what triggered diagnosis */
        if (last_slot->unchanged_cnt % OBWDG_TIMEOUT != 0) continue;

        // TODO: more advanced diagnosis: get reason for the wait
        for (size_t j = 0; j < args->info_slot_used; ++j) {
            if (args->info_slots[j].counter_slot_idx == i) {
                unsigned long argv[] = { i, };
                orbit_scratch_push_operation(&scratch, obwdg_helper_print, 1, argv);
            }
        }
    }

    ret = orbit_sendv(&scratch);
    assert(ret == 0);

    return 0;
}

/* A background thread to periodically schedule orbit check */
static void *obwdg_bg_loop(void *aux) {
    (void)aux;
    int check_type = 0;
    while (true) {
        struct orbit_task task;
        union orbit_result result;
        int ret;

        if (check_type == 1) {
            /* diagnosis */
            struct obwdg_diagnosis_args args = {
                .info_slots = obwdg_info_slots,
                .counter_slot_used = counter_slot_used,
                .info_slot_used = info_slot_used,
            };
            ret = orbit_call_async(obwdg, 0, 1, &obwdg_info_pool,
                obwdg_diagnosis, &args, sizeof(args), &task);
        } else {
            /* normal check */
            struct obwdg_entry_args args = {
                .counter_slots = obwdg_counter_slots,
                .counter_slot_used = counter_slot_used,
            };
            ret = orbit_call_async(obwdg, 0, 1, &obwdg_counter_pool,
                obwdg_entry, &args, sizeof(args), &task);
        }
        if (ret < 0) {
            int err = errno;
            fprintf(stderr, "orbit: error making orbit call: err=%d, %s\n", err, strerror(err));
            // TODO: error handling
            abort();
        }

        /* Apply set to all counters */
        ret = orbit_recvv(&result, &task);
        assert(ret == 1);
        orbit_apply(&result.scratch, false);

        /* Receive return value to determine whether we need a diagnosis run */
        ret = orbit_recvv(&result, &task);
        assert(ret == 0);
        assert(result.retval == 0 || result.retval == 1);
        check_type = result.retval;
        if (check_type == 0)
            sleep(1);
    }
}

static apr_status_t thread_mutex_cleanup(void *data)
{
    apr_thread_mutex_t *mutex = data;
    apr_status_t rv;

    release_info_slot(mutex->obwdginfo);

    rv = pthread_mutex_destroy(&mutex->mutex);
#ifdef HAVE_ZOS_PTHREADS
    if (rv) {
        rv = errno;
    }
#endif
    return rv;
} 

APR_DECLARE(apr_status_t) apr_thread_mutex_create(apr_thread_mutex_t **mutex,
                                                  unsigned int flags,
                                                  apr_pool_t *pool)
{
    apr_thread_mutex_t *new_mutex;
    apr_status_t rv;
    
#ifndef HAVE_PTHREAD_MUTEX_RECURSIVE
    if (flags & APR_THREAD_MUTEX_NESTED) {
        return APR_ENOTIMPL;
    }
#endif

    new_mutex = apr_pcalloc(pool, sizeof(apr_thread_mutex_t));
    new_mutex->pool = pool;

#ifdef HAVE_PTHREAD_MUTEX_RECURSIVE
    if (flags & APR_THREAD_MUTEX_NESTED) {
        pthread_mutexattr_t mattr;
        
        rv = pthread_mutexattr_init(&mattr);
        if (rv) return rv;
        
        rv = pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
        if (rv) {
            pthread_mutexattr_destroy(&mattr);
            return rv;
        }
         
        rv = pthread_mutex_init(&new_mutex->mutex, &mattr);
        
        pthread_mutexattr_destroy(&mattr);
    } else
#endif
        rv = pthread_mutex_init(&new_mutex->mutex, NULL);

    if (rv) {
#ifdef HAVE_ZOS_PTHREADS
        rv = errno;
#endif
        return rv;
    }

#ifndef HAVE_PTHREAD_MUTEX_TIMEDLOCK
    if (flags & APR_THREAD_MUTEX_TIMED) {
        rv = apr_thread_cond_create(&new_mutex->cond, pool);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            pthread_mutex_destroy(&new_mutex->mutex);
            return rv;
        }
    }
#endif

    apr_pool_cleanup_register(new_mutex->pool,
                              new_mutex, thread_mutex_cleanup,
                              apr_pool_cleanup_null);

    new_mutex->obwdginfo = grab_info_slot();

    *mutex = new_mutex;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_thread_mutex_lock(apr_thread_mutex_t *mutex)
{
    apr_status_t rv;

    if (mutex->cond) {
        apr_status_t rv2;

        rv = pthread_mutex_lock(&mutex->mutex);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            return rv;
        }

        if (mutex->locked) {
            mutex->num_waiters++;
            rv = apr_thread_cond_wait(mutex->cond, mutex);
            mutex->num_waiters--;
        }
        else {
            mutex->locked = 1;
            obwdg_acquired(mutex);
        }

        rv2 = pthread_mutex_unlock(&mutex->mutex);
        if (rv2 && !rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#else
            rv = rv2;
#endif
        }

        return rv;
    }

    rv = pthread_mutex_lock(&mutex->mutex);
#ifdef HAVE_ZOS_PTHREADS
    if (rv) {
        rv = errno;
    }
#endif
    obwdg_acquired(mutex);

    return rv;
}

APR_DECLARE(apr_status_t) apr_thread_mutex_trylock(apr_thread_mutex_t *mutex)
{
    apr_status_t rv;

    if (mutex->cond) {
        apr_status_t rv2;

        rv = pthread_mutex_lock(&mutex->mutex);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            return rv;
        }

        if (mutex->locked) {
            rv = APR_EBUSY;
        }
        else {
            mutex->locked = 1;
            obwdg_acquired(mutex);
        }

        rv2 = pthread_mutex_unlock(&mutex->mutex);
        if (rv2) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#else
            rv = rv2;
#endif
        }

        return rv;
    }

    rv = pthread_mutex_trylock(&mutex->mutex);
    if (rv) {
#ifdef HAVE_ZOS_PTHREADS
        rv = errno;
#endif
        return (rv == EBUSY) ? APR_EBUSY : rv;
    }
    obwdg_acquired(mutex);

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_thread_mutex_timedlock(apr_thread_mutex_t *mutex,
                                                 apr_interval_time_t timeout)
{
    apr_status_t rv = APR_ENOTIMPL;
#if APR_HAS_TIMEDLOCKS

#ifdef HAVE_PTHREAD_MUTEX_TIMEDLOCK
    if (timeout <= 0) {
        rv = pthread_mutex_trylock(&mutex->mutex);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            if (rv == EBUSY) {
                rv = APR_TIMEUP;
            }
        } else {
            obwdg_acquired(mutex);
        }
    }
    else {
        struct timespec abstime;

        timeout += apr_time_now();
        abstime.tv_sec = apr_time_sec(timeout);
        abstime.tv_nsec = apr_time_usec(timeout) * 1000; /* nanoseconds */

        rv = pthread_mutex_timedlock(&mutex->mutex, &abstime);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            if (rv == ETIMEDOUT) {
                rv = APR_TIMEUP;
            }
        } else {
            obwdg_acquired(mutex);
        }
    }

#else /* HAVE_PTHREAD_MUTEX_TIMEDLOCK */

    if (mutex->cond) {
        rv = pthread_mutex_lock(&mutex->mutex);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            return rv;
        }

        if (mutex->locked) {
            if (timeout <= 0) {
                rv = APR_TIMEUP;
            }
            else {
                mutex->num_waiters++;
                do {
                    rv = apr_thread_cond_timedwait(mutex->cond, mutex,
                                                   timeout);
                    if (rv) {
#ifdef HAVE_ZOS_PTHREADS
                        rv = errno;
#endif
                        break;
                    }
                } while (mutex->locked);
                mutex->num_waiters--;
            }
            if (rv) {
                pthread_mutex_unlock(&mutex->mutex);
                return rv;
            }
        }

        mutex->locked = 1;
        obwdg_acquired(mutex);

        rv = pthread_mutex_unlock(&mutex->mutex);
        if (rv) {
#ifdef HAVE_ZOS_PTHREADS
            rv = errno;
#endif
            return rv;
        }
    }

#endif /* HAVE_PTHREAD_MUTEX_TIMEDLOCK */

#endif /* APR_HAS_TIMEDLOCKS */
    return rv;
}

APR_DECLARE(apr_status_t) apr_thread_mutex_unlock(apr_thread_mutex_t *mutex)
{
    apr_status_t status;
    bool a_path = !!(mutex->cond);

    if (mutex->cond) {
        status = pthread_mutex_lock(&mutex->mutex);
        if (status) {
#ifdef HAVE_ZOS_PTHREADS
            status = errno;
#endif
            return status;
        }

        if (!mutex->locked) {
            status = APR_EINVAL;
        }
        else if (mutex->num_waiters) {
            status = apr_thread_cond_signal(mutex->cond);
        }
        if (status) {
            pthread_mutex_unlock(&mutex->mutex);
            return status;
        }

        mutex->locked = 0;
        obwdg_released(mutex);
    }

    status = pthread_mutex_unlock(&mutex->mutex);
#ifdef HAVE_ZOS_PTHREADS
    if (status) {
        status = errno;
    }
#endif
    if (!status && !a_path)
        obwdg_released(mutex);

    return status;
}

APR_DECLARE(apr_status_t) apr_thread_mutex_destroy(apr_thread_mutex_t *mutex)
{
    apr_status_t rv, rv2 = APR_SUCCESS;

    if (mutex->cond) {
        rv2 = apr_thread_cond_destroy(mutex->cond);
    }
    rv = apr_pool_cleanup_run(mutex->pool, mutex, thread_mutex_cleanup);
    if (rv == APR_SUCCESS) {
        rv = rv2;
    }
    
    return rv;
}

APR_POOL_IMPLEMENT_ACCESSOR(thread_mutex)

#endif /* APR_HAS_THREADS */
