/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/future.h>

#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/io/channel.h>
#include <aws/io/event_loop.h>

enum aws_future_type {
    AWS_FUTURE_T_BY_VALUE,
    AWS_FUTURE_T_BY_VALUE_WITH_CLEAN_UP,
    AWS_FUTURE_T_POINTER,
    AWS_FUTURE_T_POINTER_WITH_DESTROY,
    AWS_FUTURE_T_POINTER_WITH_RELEASE,
};

struct aws_future_callback_data {
    aws_future_callback_fn *fn;
    void *user_data;
    union aws_future_callback_union {
        struct aws_event_loop *event_loop;
        struct aws_channel *channel;
    } u;
    enum aws_future_callback_type {
        AWS_FUTURE_IMMEDIATE_CALLBACK,
        AWS_FUTURE_EVENT_LOOP_CALLBACK,
        AWS_FUTURE_CHANNEL_CALLBACK,
    } type;
};

/* When allocating aws_future<T> on the heap, we make 1 allocation containing:
 * aws_future_impl followed by T */
struct aws_future_impl {
    struct aws_allocator *alloc;
    struct aws_ref_count ref_count;
    struct aws_mutex lock;
    struct aws_condition_variable wait_cvar;
    struct aws_future_callback_data callback;
    union {
        aws_future_impl_result_clean_up_fn *clean_up;
        aws_future_impl_result_destroy_fn *destroy;
        aws_future_impl_result_release_fn *release;
    } result_dtor;
    int error_code;
    /* sum of bit fields should be 32 */
#define BIT_COUNT_FOR_SIZEOF_RESULT 27
    unsigned int sizeof_result : BIT_COUNT_FOR_SIZEOF_RESULT;
    unsigned int type : 3; /* aws_future_type */
    unsigned int is_done : 1;
    unsigned int owns_result : 1;
};

static void s_future_impl_result_dtor(struct aws_future_impl *future, void *result_addr) {

/*
 * On ARM machines, the compiler complains about the array bounds warning for aws_future_bool, even though
 * aws_future_bool will never go into any destroy or release branch. Disable the warning since it's a false positive.
 */
#ifndef _MSC_VER
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    switch (future->type) {
        case AWS_FUTURE_T_BY_VALUE_WITH_CLEAN_UP: {
            future->result_dtor.clean_up(result_addr);
            break;
        } break;

        case AWS_FUTURE_T_POINTER_WITH_DESTROY: {
            void *result = *(void **)result_addr;
            if (result) {
                future->result_dtor.destroy(result);
            }
        } break;

        case AWS_FUTURE_T_POINTER_WITH_RELEASE: {
            void *result = *(void **)result_addr;
            if (result) {
                future->result_dtor.release(result);
            }
        } break;

        default:
            break;
    }
#ifndef _MSC_VER
#    pragma GCC diagnostic pop
#endif
}

static void s_future_impl_destroy(void *user_data) {
    struct aws_future_impl *future = user_data;
    if (future->owns_result && !future->error_code) {
        s_future_impl_result_dtor(future, aws_future_impl_get_result_address(future));
    }
    aws_condition_variable_clean_up(&future->wait_cvar);
    aws_mutex_clean_up(&future->lock);
    aws_mem_release(future->alloc, future);
}

static struct aws_future_impl *s_future_impl_new(struct aws_allocator *alloc, size_t sizeof_result) {
    size_t total_size = sizeof(struct aws_future_impl) + sizeof_result;
    struct aws_future_impl *future = aws_mem_calloc(alloc, 1, total_size);
    future->alloc = alloc;

    /* we store sizeof_result in a bit field, ensure the number will fit */
    AWS_ASSERT(sizeof_result <= (UINT_MAX >> (32 - BIT_COUNT_FOR_SIZEOF_RESULT)));
    future->sizeof_result = (unsigned int)sizeof_result;

    aws_ref_count_init(&future->ref_count, future, s_future_impl_destroy);
    aws_mutex_init(&future->lock);
    aws_condition_variable_init(&future->wait_cvar);
    return future;
}

struct aws_future_impl *aws_future_impl_new_by_value(struct aws_allocator *alloc, size_t sizeof_result) {
    struct aws_future_impl *future = s_future_impl_new(alloc, sizeof_result);
    future->type = AWS_FUTURE_T_BY_VALUE;
    return future;
}

struct aws_future_impl *aws_future_impl_new_by_value_with_clean_up(
    struct aws_allocator *alloc,
    size_t sizeof_result,
    aws_future_impl_result_clean_up_fn *result_clean_up) {

    AWS_ASSERT(result_clean_up);
    struct aws_future_impl *future = s_future_impl_new(alloc, sizeof_result);
    future->type = AWS_FUTURE_T_BY_VALUE_WITH_CLEAN_UP;
    future->result_dtor.clean_up = result_clean_up;
    return future;
}

struct aws_future_impl *aws_future_impl_new_pointer(struct aws_allocator *alloc) {
    struct aws_future_impl *future = s_future_impl_new(alloc, sizeof(void *));
    future->type = AWS_FUTURE_T_POINTER;
    return future;
}

struct aws_future_impl *aws_future_impl_new_pointer_with_destroy(
    struct aws_allocator *alloc,
    aws_future_impl_result_destroy_fn *result_destroy) {

    AWS_ASSERT(result_destroy);
    struct aws_future_impl *future = s_future_impl_new(alloc, sizeof(void *));
    future->type = AWS_FUTURE_T_POINTER_WITH_DESTROY;
    future->result_dtor.destroy = result_destroy;
    return future;
}

struct aws_future_impl *aws_future_impl_new_pointer_with_release(
    struct aws_allocator *alloc,
    aws_future_impl_result_release_fn *result_release) {

    AWS_ASSERT(result_release);
    struct aws_future_impl *future = s_future_impl_new(alloc, sizeof(void *));
    future->type = AWS_FUTURE_T_POINTER_WITH_RELEASE;
    future->result_dtor.release = result_release;
    return future;
}

struct aws_future_impl *aws_future_impl_release(struct aws_future_impl *future) {
    if (future != NULL) {
        aws_ref_count_release(&future->ref_count);
    }
    return NULL;
}

struct aws_future_impl *aws_future_impl_acquire(struct aws_future_impl *future) {
    if (future != NULL) {
        aws_ref_count_acquire(&future->ref_count);
    }
    return future;
}

bool aws_future_impl_is_done(const struct aws_future_impl *future) {
    AWS_ASSERT(future);

    /* this function is conceptually const, but we need to hold the lock a moment */
    struct aws_mutex *mutable_lock = (struct aws_mutex *)&future->lock;

    /* BEGIN CRITICAL SECTION */
    aws_mutex_lock(mutable_lock);
    bool is_done = future->is_done != 0;
    aws_mutex_unlock(mutable_lock);
    /* END CRITICAL SECTION */

    return is_done;
}

int aws_future_impl_get_error(const struct aws_future_impl *future) {
    AWS_ASSERT(future != NULL);
    /* not bothering with lock, none of this can change after future is done */
    AWS_FATAL_ASSERT(future->is_done && "Cannot get error before future is done");
    return future->error_code;
}

void *aws_future_impl_get_result_address(const struct aws_future_impl *future) {
    AWS_ASSERT(future != NULL);
    /* not bothering with lock, none of this can change after future is done */
    AWS_FATAL_ASSERT(future->is_done && "Cannot get result before future is done");
    AWS_FATAL_ASSERT(!future->error_code && "Cannot get result from future that failed with an error");
    AWS_FATAL_ASSERT(future->owns_result && "Result was already moved from future");

    const struct aws_future_impl *address_of_memory_after_this_struct = future + 1;
    void *result_addr = (void *)address_of_memory_after_this_struct;
    return result_addr;
}

void aws_future_impl_get_result_by_move(struct aws_future_impl *future, void *dst_address) {
    void *result_addr = aws_future_impl_get_result_address(future);
    memcpy(dst_address, result_addr, future->sizeof_result);
    memset(result_addr, 0, future->sizeof_result);
    future->owns_result = false;
}

/* Data for invoking callback as a task on an event-loop */
struct aws_future_event_loop_callback_job {
    struct aws_allocator *alloc;
    struct aws_task task;
    struct aws_event_loop *event_loop;
    aws_future_callback_fn *callback;
    void *user_data;
};

static void s_future_impl_event_loop_callback_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;
    struct aws_future_event_loop_callback_job *job = arg;
    job->callback(job->user_data);
    // TODO: aws_event_loop_release(job->event_loop);
    aws_mem_release(job->alloc, job);
}

/* Data for invoking callback as a task on an aws_channel */
struct aws_future_channel_callback_job {
    struct aws_allocator *alloc;
    struct aws_channel_task task;
    struct aws_channel *channel;
    aws_future_callback_fn *callback;
    void *user_data;
};

static void s_future_impl_channel_callback_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;
    struct aws_future_channel_callback_job *job = arg;
    job->callback(job->user_data);
    aws_channel_release_hold(job->channel);
    aws_mem_release(job->alloc, job);
}

static void s_future_impl_invoke_callback(struct aws_future_callback_data *callback, struct aws_allocator *alloc) {
    AWS_ASSERT(callback->fn);

    switch (callback->type) {
        case AWS_FUTURE_IMMEDIATE_CALLBACK: {
            callback->fn(callback->user_data);
        } break;

        case AWS_FUTURE_EVENT_LOOP_CALLBACK: {
            /* Schedule the callback as a task on the event-loop */
            struct aws_future_event_loop_callback_job *job =
                aws_mem_calloc(alloc, 1, sizeof(struct aws_future_event_loop_callback_job));
            job->alloc = alloc;
            aws_task_init(&job->task, s_future_impl_event_loop_callback_task, job, "aws_future_event_loop_callback");
            job->event_loop = callback->u.event_loop;
            job->callback = callback->fn;
            job->user_data = callback->user_data;

            aws_event_loop_schedule_task_now(callback->u.event_loop, &job->task);
        } break;

        case AWS_FUTURE_CHANNEL_CALLBACK: {
            /* Schedule the callback as a task on the channel */
            struct aws_future_channel_callback_job *job =
                aws_mem_calloc(alloc, 1, sizeof(struct aws_future_channel_callback_job));
            job->alloc = alloc;
            aws_channel_task_init(&job->task, s_future_impl_channel_callback_task, job, "aws_future_channel_callback");
            job->channel = callback->u.channel;
            job->callback = callback->fn;
            job->user_data = callback->user_data;

            aws_channel_schedule_task_now(callback->u.channel, &job->task);
        } break;
    }
}

static void s_future_impl_set_done(struct aws_future_impl *future, void *src_address, int error_code) {
    bool is_error = error_code != 0;

    /* BEGIN CRITICAL SECTION */
    aws_mutex_lock(&future->lock);

    struct aws_future_callback_data callback = future->callback;

    bool first_time = !future->is_done;
    if (first_time) {
        future->is_done = true;
        AWS_ZERO_STRUCT(future->callback);
        if (is_error) {
            future->error_code = error_code;
        } else {
            future->owns_result = true;
            AWS_FATAL_ASSERT(src_address != NULL);
            memcpy(aws_future_impl_get_result_address(future), src_address, future->sizeof_result);
        }

        aws_condition_variable_notify_all(&future->wait_cvar);
    }

    aws_mutex_unlock(&future->lock);
    /* END CRITICAL SECTION */

    if (first_time) {
        /* if callback was registered, invoke it now, outside of critical section to avoid deadlock */
        if (callback.fn != NULL) {
            s_future_impl_invoke_callback(&callback, future->alloc);
        }
    } else if (!error_code) {
        /* future was already done, so just destroy this newer result */
        s_future_impl_result_dtor(future, src_address);
    }
}

void aws_future_impl_set_error(struct aws_future_impl *future, int error_code) {
    AWS_ASSERT(future);

    /* handle recoverable usage error */
    AWS_ASSERT(error_code != 0);
    if (AWS_UNLIKELY(error_code == 0)) {
        error_code = AWS_ERROR_UNKNOWN;
    }

    s_future_impl_set_done(future, NULL /*src_address*/, error_code);
}

void aws_future_impl_set_result_by_move(struct aws_future_impl *future, void *src_address) {
    AWS_ASSERT(future);
    AWS_ASSERT(src_address);
    s_future_impl_set_done(future, src_address, 0 /*error_code*/);

    /* the future takes ownership of the result.
     * zero out memory at the src_address to reinforce this transfer of ownership. */
    memset(src_address, 0, future->sizeof_result);
}

/* Returns true if callback was registered, or false if callback was ignored
 * because the the future is already done and invoke_if_already_done==false */
static bool s_future_impl_register_callback(
    struct aws_future_impl *future,
    struct aws_future_callback_data *callback,
    bool invoke_if_already_done) {

    /* BEGIN CRITICAL SECTION */
    aws_mutex_lock(&future->lock);

    AWS_FATAL_ASSERT(future->callback.fn == NULL && "Future done callback must only be set once");

    bool already_done = future->is_done != 0;

    /* if not done, store callback for later */
    if (!already_done) {
        future->callback = *callback;
    }

    aws_mutex_unlock(&future->lock);
    /* END CRITICAL SECTION */

    /* if already done, invoke callback now */
    if (already_done && invoke_if_already_done) {
        s_future_impl_invoke_callback(callback, future->alloc);
    }

    return !already_done || invoke_if_already_done;
}

void aws_future_impl_register_callback(
    struct aws_future_impl *future,
    aws_future_callback_fn *on_done,
    void *user_data) {

    AWS_ASSERT(future);
    AWS_ASSERT(on_done);

    struct aws_future_callback_data callback = {
        .fn = on_done,
        .user_data = user_data,
        .type = AWS_FUTURE_IMMEDIATE_CALLBACK,
    };
    s_future_impl_register_callback(future, &callback, true /*invoke_if_already_done*/);
}

bool aws_future_impl_register_callback_if_not_done(
    struct aws_future_impl *future,
    aws_future_callback_fn *on_done,
    void *user_data) {

    AWS_ASSERT(future);
    AWS_ASSERT(on_done);

    struct aws_future_callback_data callback = {
        .fn = on_done,
        .user_data = user_data,
        .type = AWS_FUTURE_IMMEDIATE_CALLBACK,
    };
    return s_future_impl_register_callback(future, &callback, false /*invoke_if_already_done*/);
}

void aws_future_impl_register_event_loop_callback(
    struct aws_future_impl *future,
    struct aws_event_loop *event_loop,
    aws_future_callback_fn *on_done,
    void *user_data) {

    AWS_ASSERT(future);
    AWS_ASSERT(event_loop);
    AWS_ASSERT(on_done);

    // TODO: aws_event_loop_acquire(event_loop);

    struct aws_future_callback_data callback = {
        .fn = on_done,
        .user_data = user_data,
        .type = AWS_FUTURE_EVENT_LOOP_CALLBACK,
        .u = {.event_loop = event_loop},
    };
    s_future_impl_register_callback(future, &callback, true /*invoke_if_already_done*/);
}

void aws_future_impl_register_channel_callback(
    struct aws_future_impl *future,
    struct aws_channel *channel,
    aws_future_callback_fn *on_done,
    void *user_data) {

    AWS_ASSERT(future);
    AWS_ASSERT(channel);
    AWS_ASSERT(on_done);

    aws_channel_acquire_hold(channel);

    struct aws_future_callback_data callback = {
        .fn = on_done,
        .user_data = user_data,
        .type = AWS_FUTURE_CHANNEL_CALLBACK,
        .u = {.channel = channel},
    };
    s_future_impl_register_callback(future, &callback, true /*invoke_if_already_done*/);
}

static bool s_future_impl_is_done_pred(void *user_data) {
    struct aws_future_impl *future = user_data;
    return future->is_done != 0;
}

bool aws_future_impl_wait(const struct aws_future_impl *future, uint64_t timeout_ns) {
    AWS_ASSERT(future);

    /* this function is conceptually const, but we need to use synchronization primitives */
    struct aws_future_impl *mutable_future = (struct aws_future_impl *)future;

    /* condition-variable takes signed timeout, so clamp to INT64_MAX (292+ years) */
    int64_t timeout_i64 = aws_min_u64(timeout_ns, INT64_MAX);

    /* BEGIN CRITICAL SECTION */
    aws_mutex_lock(&mutable_future->lock);

    bool is_done = aws_condition_variable_wait_for_pred(
                       &mutable_future->wait_cvar,
                       &mutable_future->lock,
                       timeout_i64,
                       s_future_impl_is_done_pred,
                       mutable_future) == AWS_OP_SUCCESS;

    aws_mutex_unlock(&mutable_future->lock);
    /* END CRITICAL SECTION */

    return is_done;
}

AWS_FUTURE_T_BY_VALUE_IMPLEMENTATION(aws_future_bool, bool)

AWS_FUTURE_T_BY_VALUE_IMPLEMENTATION(aws_future_size, size_t)

/**
 * aws_future<void>
 */
AWS_FUTURE_T_IMPLEMENTATION_BEGIN(aws_future_void)

struct aws_future_void *aws_future_void_new(struct aws_allocator *alloc) {
    /* Use aws_future<bool> under the hood, to avoid edge-cases with 0-sized result */
    return (struct aws_future_void *)aws_future_bool_new(alloc);
}

void aws_future_void_set_result(struct aws_future_void *future) {
    aws_future_bool_set_result((struct aws_future_bool *)future, false);
}

AWS_FUTURE_T_IMPLEMENTATION_END(aws_future_void)
