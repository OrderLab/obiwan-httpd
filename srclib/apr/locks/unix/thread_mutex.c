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
#define OBWDG_TIMEOUT 60
// #define OBWDG_TIMEOUT 10

#if 0
#define obprintf(fmt, ...) do { \
        fprintf(fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define obprintf(fmt, ...) do { } while (0)
#endif

/* Watchdog counter that is reset by the orbit and inc by the main program. */
static struct ob_watchdog_counter_slot {
    /* Generation number.
     * Each time this slot is reused, the slot will be incremented twice:
     * odd for grabbing the slot, even for releasing it. This value is allowed
     * to safely overflow. */
    uint16_t gen;
    /* Counter for current generation */
    uint16_t counter;
    /* Number of held locks */
    uint16_t nholds;
} *obwdg_counter_slots = NULL;
static size_t obwdg_counter_last = 0;

/* Watchdog debug information that can be used for full diagnosis */
static struct ob_mutex_watchdog_info {
    /* For diagnosis, we make an assumption that a lock will always be acquired
     * and released by the same thread. */
    pid_t holder_tid;
    time_t time;
    size_t counter_slot_idx;
    uint16_t gen;
    apr_thread_mutex_t *mutex;
    const struct call_site *creator_loc;
    const struct call_site *holder_loc;
} *obwdg_info_slots = NULL;
static size_t obwdg_info_last = 0;

const struct call_site unknown_loc = { "unknown", "unknown", 0, };

#define INVIDX ((size_t)-1)
static __thread size_t counter_slot_idx = INVIDX;
static __thread pid_t current_tid = 0;

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

static void dump_counter(struct ob_watchdog_counter_slot *slot)
{
    fprintf(stderr, "counter info: id=%ld, counter=%hu, holder=%hu\n",
            slot - obwdg_counter_slots, slot->counter, slot->nholds);
}

static void dump_wdg_info(struct ob_mutex_watchdog_info *info)
{
    const struct call_site *creator_loc = info->creator_loc ?: &unknown_loc;
    const struct call_site *holder_loc = info->holder_loc ?: &unknown_loc;
    fprintf(stderr, "lock info: id=%ld, tid=%d\n"
            "\t time=%ld, slotidx=%lu mutex=%p\n"
            "\t creator = %s:%d:%s\n"
            "\t holder = %s:%d:%s\n",
            info - obwdg_info_slots, info->holder_tid,
            info->time, info->counter_slot_idx, info->mutex,
            creator_loc->file, creator_loc->line, creator_loc->func,
            holder_loc->file, holder_loc->line, holder_loc->func);
}

void obwdg_init() {
    if (obwdg_info_alloc != NULL) return;
    obwdg_counter_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_info_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_scratch_pool = orbit_pool_create(NULL, 64 * 1024 * 1024);
    obwdg_counter_pool->mode = ORBIT_COPY;
    obwdg_info_pool->mode = ORBIT_COPY;
    obwdg_counter_alloc = orbit_allocator_from_pool(obwdg_counter_pool, true);
    obwdg_info_alloc = orbit_allocator_from_pool(obwdg_info_pool, true);
    orbit_scratch_set_pool(obwdg_scratch_pool);

    obwdg_counter_slots = orbit_calloc(obwdg_counter_alloc,
            sizeof(struct ob_watchdog_counter_slot) * NSLOTS);
    obwdg_info_slots = orbit_calloc(obwdg_info_alloc,
            sizeof(struct ob_mutex_watchdog_info) * NSLOTS);
    for (size_t i = 0; i < NSLOTS; ++i)
        obwdg_info_slots[i].counter_slot_idx = INVIDX;

    obwdg = orbit_create("ob mutex watchdog", obwdg_entry, obwdg_create_store);
    int ret = pthread_create(&obwdg_bg, NULL, obwdg_bg_loop, NULL);
    assert(ret == 0);
}

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#define grab_slot(slot_array, start_hint, size, field) ({ \
    int start = __atomic_load_n(start_hint, __ATOMIC_RELAXED); \
    size_t slot = INVIDX; \
    for (int j = 0; j < NSLOTS; ++j) { \
        int i = (start + j) % NSLOTS;\
        uint16_t gen = __atomic_load_n(&(slot_array)[i].field, __ATOMIC_ACQUIRE); \
        if ((gen & 1) != 0) continue; \
        if (!__atomic_compare_exchange_n(&(slot_array)[i].field, \
                &gen, gen+1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) \
            continue; \
        slot = i;\
        __atomic_store_n(start_hint, i, __ATOMIC_RELAXED); \
        break; \
    } \
    slot; })

void grab_counter_slot(void) {
    if (unlikely(counter_slot_idx != INVIDX)) return;

    size_t slot = grab_slot(obwdg_counter_slots, &obwdg_counter_last, NSLOTS, gen);
    if (slot == INVIDX) {
        fprintf(stderr, "orbit: counter slots used up\n");
        abort();
    }
    obwdg_counter_slots[slot].counter = 0;
    obwdg_counter_slots[slot].nholds = 0;
    counter_slot_idx = slot;

    /* Older Linux does not have gittid wrapper */
    current_tid = syscall(SYS_gettid);
}

void release_counter_slot(void) {
    if (counter_slot_idx == INVIDX) return;
    __atomic_add_fetch(&obwdg_counter_slots[counter_slot_idx].gen, 1, __ATOMIC_RELEASE);
    counter_slot_idx = INVIDX;
}

static inline struct ob_mutex_watchdog_info *grab_info_slot() {
    size_t slot_idx = grab_slot(obwdg_info_slots, &obwdg_info_last, NSLOTS, gen);
    if (slot_idx == INVIDX) {
        fprintf(stderr, "orbit: info slots used up\n");
        abort();
    }
    obwdg_info_slots[slot_idx].counter_slot_idx = INVIDX;
    return obwdg_info_slots + slot_idx;
}

static inline void release_info_slot(struct ob_mutex_watchdog_info *info) {
    info->counter_slot_idx = INVIDX;
    __atomic_add_fetch(&info->gen, 1, __ATOMIC_RELEASE);
}

void obwdg_acquired(apr_thread_mutex_t *mutex, const struct call_site *loc) {
    size_t idx = counter_slot_idx;
    /* It is safe to add modify the counter and holder since the current
     * thread owns this slot. */
    ++obwdg_counter_slots[idx].counter;
    ++obwdg_counter_slots[idx].nholds;
    obprintf(stderr, "counter %lu acquiring lock %ld by tid %ld, nholds %d\n", idx,
            mutex->obwdginfo - obwdg_info_slots,
            current_tid,
            obwdg_counter_slots[idx].nholds);

    mutex->obwdginfo->holder_tid = current_tid;
    //mutex->obwdginfo->time = time(NULL);
    mutex->obwdginfo->time = 0;
    mutex->obwdginfo->counter_slot_idx = idx;
    mutex->obwdginfo->mutex = mutex;
    mutex->obwdginfo->holder_loc = loc;
}
#define obwdg_acquired(mutex) obwdg_acquired(mutex, loc)

void obwdg_released(apr_thread_mutex_t *mutex, const struct call_site *loc) {
    size_t idx = counter_slot_idx;
    ++obwdg_counter_slots[idx].counter;
    --obwdg_counter_slots[idx].nholds;
    obprintf(stderr, "counter %lu releasing lock %ld by tid %ld, nholds %d\n", idx,
            mutex->obwdginfo - obwdg_info_slots,
            syscall(SYS_gettid), obwdg_counter_slots[idx].nholds);

    mutex->obwdginfo->holder_tid = 0;
    //mutex->obwdginfo->time = time(NULL);
    mutex->obwdginfo->time = 0;
    mutex->obwdginfo->counter_slot_idx = INVIDX;
    mutex->obwdginfo->mutex = NULL;
    mutex->obwdginfo->holder_loc = NULL;
}
#define obwdg_released(mutex) obwdg_released(mutex, loc)

struct obwdg_store {
    struct slot_stats {
        uint16_t nholds;
        uint16_t gen;
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

static unsigned long obwdg_helper_print(size_t nargs, unsigned long argv[]) {
    size_t counter_idx = argv[0];
    size_t info_idx = argv[1];
    int unchanged = argv[2];
    struct ob_mutex_watchdog_info *info = obwdg_info_slots + info_idx;
    fprintf(stderr, "orbit: counter %lu has been occupied by lock %lu for %d checks\n",
            counter_idx, info_idx, unchanged);
    dump_counter(obwdg_counter_slots + counter_idx);
    dump_wdg_info(obwdg_info_slots + info_idx);
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
    obprintf(stderr, "Begin obwdg\n");

    for (int i = 0; i < args->counter_slot_used; ++i) {
        struct slot_stats *last_slot = store->last_values + i;
        struct ob_watchdog_counter_slot *curr_slot = args->counter_slots + i;

        if (curr_slot->gen == last_slot->gen && curr_slot->nholds != 0
		&& curr_slot->nholds == last_slot->nholds && curr_slot->counter == 0)
        {
            /* The counter hasn't been changed for 1 sec. Note down this in
             * unchanged_cnt. */
            if (++last_slot->unchanged_cnt % OBWDG_TIMEOUT == 0) {
                obprintf(stderr, "holders %d\n", curr_slot->nholds);
                /* We should trigger a diagnosis held for 1 min. */
                next_check = 1;
                obprintf(stderr, "orbit: found slot %d to have long running thread\n", i);
            }
        } else {
            /* Either nholds or counter has been changed, reset the slot. */
            last_slot->nholds = curr_slot->nholds;
            last_slot->gen = curr_slot->gen;
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
    obprintf(stderr, "End obwdg\n");

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
        if (last_slot->unchanged_cnt == 0 ||
            last_slot->unchanged_cnt % OBWDG_TIMEOUT != 0)
            continue;

        // TODO: more advanced diagnosis: get reason for the wait
        for (size_t j = 0; j < args->info_slot_used; ++j) {
            if (args->info_slots[j].counter_slot_idx == i) {
                // TODO: change this to copy the whole `info` struct to
                // operation with new API.
                unsigned long argv[] = { i, j, last_slot->unchanged_cnt, };
                orbit_scratch_push_operation(&scratch, obwdg_helper_print, 3, argv);
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
#if 0
            for (int i = 0; i < counter_slot_used; ++i)
                dump_counter(obwdg_counter_slots + i);
            for (int i = 0; i < info_slot_used; ++i)
                dump_wdg_info(obwdg_info_slots + i);
#endif
            struct obwdg_diagnosis_args args = {
                .info_slots = obwdg_info_slots,
                .counter_slot_used = NSLOTS,
                .info_slot_used = NSLOTS,
            };
            ret = orbit_call_async(obwdg, 0, 1, &obwdg_info_pool,
                obwdg_diagnosis, &args, sizeof(args), &task);
        } else {
            /* normal check */
            struct obwdg_entry_args args = {
                .counter_slots = obwdg_counter_slots,
                .counter_slot_used = NSLOTS,
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

    mutex->obwdginfo->creator_loc = NULL;
    release_info_slot(mutex->obwdginfo);

    rv = pthread_mutex_destroy(&mutex->mutex);
#ifdef HAVE_ZOS_PTHREADS
    if (rv) {
        rv = errno;
    }
#endif
    return rv;
} 

APR_DECLARE(apr_status_t) __apr_thread_mutex_create(apr_thread_mutex_t **mutex,
        unsigned int flags, apr_pool_t *pool, const struct call_site *loc)
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
    new_mutex->obwdginfo->creator_loc = loc;
    obprintf(stderr, "orbit: mutex %p created at %s:%d:%s\n", new_mutex,
             loc->file, loc->line, loc->func);

    *mutex = new_mutex;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) __apr_thread_mutex_lock(apr_thread_mutex_t *mutex,
        const struct call_site *loc)
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
    if (rv) {
#ifdef HAVE_ZOS_PTHREADS
        rv = errno;
#endif
    } else {
        obwdg_acquired(mutex);
    }

    return rv;
}

APR_DECLARE(apr_status_t) __apr_thread_mutex_trylock(apr_thread_mutex_t *mutex,
        const struct call_site *loc)
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
    } else {
        obwdg_acquired(mutex);
    }

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) __apr_thread_mutex_timedlock(apr_thread_mutex_t *mutex,
        apr_interval_time_t timeout, const struct call_site *loc)
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

APR_DECLARE(apr_status_t) __apr_thread_mutex_unlock(apr_thread_mutex_t *mutex,
        const struct call_site *loc)
{
    apr_status_t status;
    obprintf(stderr, "tid %ld try unlock %p\n", syscall(SYS_gettid), mutex);

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
    }
    obwdg_released(mutex);

    status = pthread_mutex_unlock(&mutex->mutex);
#ifdef HAVE_ZOS_PTHREADS
    if (status) {
        status = errno;
    }
#endif

    return status;
}

APR_DECLARE(apr_status_t) __apr_thread_mutex_destroy(apr_thread_mutex_t *mutex,
        const struct call_site *loc)
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
