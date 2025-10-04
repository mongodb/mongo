/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/retry_strategy.h>

#include <aws/io/event_loop.h>
#include <aws/io/logging.h>

#include <aws/common/clock.h>
#include <aws/common/device_random.h>
#include <aws/common/mutex.h>
#include <aws/common/shutdown_types.h>
#include <aws/common/task_scheduler.h>

#include <inttypes.h>

struct exponential_backoff_strategy {
    struct aws_retry_strategy base;
    struct aws_exponential_backoff_retry_options config;
    struct aws_shutdown_callback_options shutdown_options;
};

struct exponential_backoff_retry_token {
    struct aws_retry_token base;
    struct aws_atomic_var current_retry_count;
    struct aws_atomic_var last_backoff;
    size_t max_retries;
    uint64_t backoff_scale_factor_ns;
    uint64_t maximum_backoff_ns;
    enum aws_exponential_backoff_jitter_mode jitter_mode;
    /* Let's not make this worse by constantly moving across threads if we can help it */
    struct aws_event_loop *bound_loop;
    uint64_t (*generate_random)(void);
    aws_generate_random_fn *generate_random_impl;
    void *generate_random_user_data;
    struct aws_task retry_task;

    struct {
        struct aws_mutex mutex;
        aws_retry_strategy_on_retry_token_acquired_fn *acquired_fn;
        aws_retry_strategy_on_retry_ready_fn *retry_ready_fn;
        void *user_data;
    } thread_data;
};

static void s_exponential_retry_destroy(struct aws_retry_strategy *retry_strategy) {
    if (retry_strategy) {
        struct exponential_backoff_strategy *exponential_strategy = retry_strategy->impl;
        struct aws_event_loop_group *el_group = exponential_strategy->config.el_group;
        aws_simple_completion_callback *completion_callback =
            exponential_strategy->shutdown_options.shutdown_callback_fn;
        void *completion_user_data = exponential_strategy->shutdown_options.shutdown_callback_user_data;

        aws_mem_release(retry_strategy->allocator, exponential_strategy);
        if (completion_callback != NULL) {
            completion_callback(completion_user_data);
        }
        aws_event_loop_group_release(el_group);
    }
}

static void s_exponential_retry_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    int error_code = AWS_ERROR_IO_OPERATION_CANCELLED;
    if (status == AWS_TASK_STATUS_RUN_READY) {
        error_code = AWS_OP_SUCCESS;
    }

    struct exponential_backoff_retry_token *backoff_retry_token = arg;
    aws_retry_strategy_on_retry_token_acquired_fn *acquired_fn = NULL;
    aws_retry_strategy_on_retry_ready_fn *retry_ready_fn = NULL;
    void *user_data = NULL;

    { /***** BEGIN CRITICAL SECTION *********/
        AWS_FATAL_ASSERT(
            !aws_mutex_lock(&backoff_retry_token->thread_data.mutex) && "Retry token mutex acquisition failed");
        acquired_fn = backoff_retry_token->thread_data.acquired_fn;
        retry_ready_fn = backoff_retry_token->thread_data.retry_ready_fn;
        user_data = backoff_retry_token->thread_data.user_data;
        backoff_retry_token->thread_data.user_data = NULL;
        backoff_retry_token->thread_data.retry_ready_fn = NULL;
        backoff_retry_token->thread_data.acquired_fn = NULL;
        AWS_FATAL_ASSERT(
            !aws_mutex_unlock(&backoff_retry_token->thread_data.mutex) && "Retry token mutex release failed");
    } /**** END CRITICAL SECTION ***********/

    aws_retry_token_acquire(&backoff_retry_token->base);
    if (acquired_fn) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
            "id=%p: Vending retry_token %p",
            (void *)backoff_retry_token->base.retry_strategy,
            (void *)&backoff_retry_token->base);
        acquired_fn(backoff_retry_token->base.retry_strategy, error_code, &backoff_retry_token->base, user_data);
    } else if (retry_ready_fn) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
            "id=%p: Invoking retry_ready for token %p",
            (void *)backoff_retry_token->base.retry_strategy,
            (void *)&backoff_retry_token->base);
        retry_ready_fn(&backoff_retry_token->base, error_code, user_data);
        /* it's acquired before being scheduled for retry */
        aws_retry_token_release(&backoff_retry_token->base);
    }
    aws_retry_token_release(&backoff_retry_token->base);
}

static int s_exponential_retry_acquire_token(
    struct aws_retry_strategy *retry_strategy,
    const struct aws_byte_cursor *partition_id,
    aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
    void *user_data,
    uint64_t timeout_ms) {
    (void)partition_id;
    /* no resource contention here so no timeouts. */
    (void)timeout_ms;

    struct exponential_backoff_retry_token *backoff_retry_token =
        aws_mem_calloc(retry_strategy->allocator, 1, sizeof(struct exponential_backoff_retry_token));

    if (!backoff_retry_token) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
        "id=%p: Initializing retry token %p",
        (void *)retry_strategy,
        (void *)&backoff_retry_token->base);

    backoff_retry_token->base.allocator = retry_strategy->allocator;
    backoff_retry_token->base.retry_strategy = retry_strategy;
    aws_atomic_init_int(&backoff_retry_token->base.ref_count, 1u);
    aws_retry_strategy_acquire(retry_strategy);
    backoff_retry_token->base.impl = backoff_retry_token;

    struct exponential_backoff_strategy *exponential_backoff_strategy = retry_strategy->impl;
    backoff_retry_token->bound_loop = aws_event_loop_group_get_next_loop(exponential_backoff_strategy->config.el_group);
    backoff_retry_token->max_retries = exponential_backoff_strategy->config.max_retries;
    backoff_retry_token->backoff_scale_factor_ns = aws_timestamp_convert(
        exponential_backoff_strategy->config.backoff_scale_factor_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
    backoff_retry_token->maximum_backoff_ns = aws_timestamp_convert(
        exponential_backoff_strategy->config.max_backoff_secs, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    backoff_retry_token->jitter_mode = exponential_backoff_strategy->config.jitter_mode;
    backoff_retry_token->generate_random = exponential_backoff_strategy->config.generate_random;
    backoff_retry_token->generate_random_impl = exponential_backoff_strategy->config.generate_random_impl;
    backoff_retry_token->generate_random_user_data = exponential_backoff_strategy->config.generate_random_user_data;

    aws_atomic_init_int(&backoff_retry_token->current_retry_count, 0);
    aws_atomic_init_int(&backoff_retry_token->last_backoff, 0);

    backoff_retry_token->thread_data.acquired_fn = on_acquired;
    backoff_retry_token->thread_data.user_data = user_data;
    AWS_FATAL_ASSERT(
        !aws_mutex_init(&backoff_retry_token->thread_data.mutex) && "Retry strategy mutex initialization failed");

    aws_task_init(
        &backoff_retry_token->retry_task,
        s_exponential_retry_task,
        backoff_retry_token,
        "aws_exponential_backoff_retry_task");
    aws_event_loop_schedule_task_now(backoff_retry_token->bound_loop, &backoff_retry_token->retry_task);

    return AWS_OP_SUCCESS;
}

static inline uint64_t s_random_in_range(uint64_t from, uint64_t to, struct exponential_backoff_retry_token *token) {
    uint64_t max = aws_max_u64(from, to);
    uint64_t min = aws_min_u64(from, to);

    uint64_t diff = max - min;

    if (!diff) {
        return 0;
    }
    uint64_t random;
    if (token->generate_random_impl) {
        random = token->generate_random_impl(token->generate_random_user_data);
    } else {
        random = token->generate_random();
    }
    return min + random % (diff);
}

typedef uint64_t(compute_backoff_fn)(struct exponential_backoff_retry_token *token);

static uint64_t s_compute_no_jitter(struct exponential_backoff_retry_token *token) {
    uint64_t retry_count = aws_min_u64(aws_atomic_load_int(&token->current_retry_count), 63);
    uint64_t backoff_ns = aws_mul_u64_saturating((uint64_t)1 << retry_count, token->backoff_scale_factor_ns);
    return aws_min_u64(backoff_ns, token->maximum_backoff_ns);
}

static uint64_t s_compute_full_jitter(struct exponential_backoff_retry_token *token) {
    uint64_t non_jittered = s_compute_no_jitter(token);
    return s_random_in_range(0, non_jittered, token);
}

static uint64_t s_compute_deccorelated_jitter(struct exponential_backoff_retry_token *token) {
    size_t last_backoff_val = aws_atomic_load_int(&token->last_backoff);

    if (!last_backoff_val) {
        return s_compute_full_jitter(token);
    }
    uint64_t backoff_ns = aws_min_u64(token->maximum_backoff_ns, aws_mul_u64_saturating(last_backoff_val, 3));
    return s_random_in_range(token->backoff_scale_factor_ns, backoff_ns, token);
}

static compute_backoff_fn *s_backoff_compute_table[] = {
    [AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT] = s_compute_full_jitter,
    [AWS_EXPONENTIAL_BACKOFF_JITTER_NONE] = s_compute_no_jitter,
    [AWS_EXPONENTIAL_BACKOFF_JITTER_FULL] = s_compute_full_jitter,
    [AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED] = s_compute_deccorelated_jitter,
};

static int s_exponential_retry_schedule_retry(
    struct aws_retry_token *token,
    enum aws_retry_error_type error_type,
    aws_retry_strategy_on_retry_ready_fn *retry_ready,
    void *user_data) {
    struct exponential_backoff_retry_token *backoff_retry_token = token->impl;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
        "id=%p: Attempting retry on token %p with error type %d",
        (void *)backoff_retry_token->base.retry_strategy,
        (void *)token,
        error_type);
    uint64_t schedule_at = 0;

    /* AWS_RETRY_ERROR_TYPE_CLIENT_ERROR does not count against your retry budget since you were responding to an
     * improperly crafted request. */
    if (error_type != AWS_RETRY_ERROR_TYPE_CLIENT_ERROR) {
        size_t retry_count = aws_atomic_load_int(&backoff_retry_token->current_retry_count);

        if (retry_count >= backoff_retry_token->max_retries) {
            AWS_LOGF_WARN(
                AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
                "id=%p: token %p has exhausted allowed retries. Retry count %zu max retries %zu",
                (void *)backoff_retry_token->base.retry_strategy,
                (void *)token,
                backoff_retry_token->max_retries,
                retry_count);
            return aws_raise_error(AWS_IO_MAX_RETRIES_EXCEEDED);
        }

        uint64_t backoff = s_backoff_compute_table[backoff_retry_token->jitter_mode](backoff_retry_token);
        uint64_t current_time = 0;

        aws_event_loop_current_clock_time(backoff_retry_token->bound_loop, &current_time);
        schedule_at = backoff + current_time;
        aws_atomic_init_int(&backoff_retry_token->last_backoff, (size_t)backoff);
        aws_atomic_fetch_add(&backoff_retry_token->current_retry_count, 1u);
        AWS_LOGF_DEBUG(
            AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
            "id=%p: Computed backoff value of %" PRIu64 "ns on token %p",
            (void *)backoff_retry_token->base.retry_strategy,
            backoff,
            (void *)token);
    }

    bool already_scheduled = false;

    { /***** BEGIN CRITICAL SECTION *********/
        AWS_FATAL_ASSERT(
            !aws_mutex_lock(&backoff_retry_token->thread_data.mutex) && "Retry token mutex acquisition failed");

        if (backoff_retry_token->thread_data.user_data) {
            already_scheduled = true;
        } else {
            backoff_retry_token->thread_data.retry_ready_fn = retry_ready;
            backoff_retry_token->thread_data.user_data = user_data;
            /* acquire to hold until the task runs. */
            aws_retry_token_acquire(token);
            aws_task_init(
                &backoff_retry_token->retry_task,
                s_exponential_retry_task,
                backoff_retry_token,
                "aws_exponential_backoff_retry_task");
        }
        AWS_FATAL_ASSERT(
            !aws_mutex_unlock(&backoff_retry_token->thread_data.mutex) && "Retry token mutex release failed");
    } /**** END CRITICAL SECTION ***********/

    if (already_scheduled) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
            "id=%p: retry token %p is already scheduled.",
            (void *)backoff_retry_token->base.retry_strategy,
            (void *)token);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    aws_event_loop_schedule_task_future(backoff_retry_token->bound_loop, &backoff_retry_token->retry_task, schedule_at);
    return AWS_OP_SUCCESS;
}

static int s_exponential_backoff_record_success(struct aws_retry_token *token) {
    /* we don't do book keeping in this mode. */
    (void)token;
    return AWS_OP_SUCCESS;
}

static void s_exponential_backoff_release_token(struct aws_retry_token *token) {
    if (token) {
        aws_retry_strategy_release(token->retry_strategy);
        struct exponential_backoff_retry_token *backoff_retry_token = token->impl;
        aws_mutex_clean_up(&backoff_retry_token->thread_data.mutex);
        aws_mem_release(token->allocator, backoff_retry_token);
    }
}

static struct aws_retry_strategy_vtable s_exponential_retry_vtable = {
    .destroy = s_exponential_retry_destroy,
    .acquire_token = s_exponential_retry_acquire_token,
    .schedule_retry = s_exponential_retry_schedule_retry,
    .record_success = s_exponential_backoff_record_success,
    .release_token = s_exponential_backoff_release_token,
};

static uint64_t s_default_gen_rand(void *user_data) {
    (void)user_data;
    uint64_t res = 0;
    aws_device_random_u64(&res);
    return res;
}

struct aws_retry_strategy *aws_retry_strategy_new_exponential_backoff(
    struct aws_allocator *allocator,
    const struct aws_exponential_backoff_retry_options *config) {
    AWS_PRECONDITION(config);
    AWS_PRECONDITION(config->el_group);
    AWS_PRECONDITION(config->jitter_mode <= AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED);
    AWS_PRECONDITION(config->max_retries);

    if (config->max_retries > 63 || !config->el_group ||
        config->jitter_mode > AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct exponential_backoff_strategy *exponential_backoff_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct exponential_backoff_strategy));

    if (!exponential_backoff_strategy) {
        return NULL;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
        "id=%p: Initializing exponential backoff retry strategy with scale factor: %" PRIu32
        " jitter mode: %d and max retries %zu",
        (void *)&exponential_backoff_strategy->base,
        config->backoff_scale_factor_ms,
        config->jitter_mode,
        config->max_retries);

    exponential_backoff_strategy->base.allocator = allocator;
    exponential_backoff_strategy->base.impl = exponential_backoff_strategy;
    exponential_backoff_strategy->base.vtable = &s_exponential_retry_vtable;
    aws_atomic_init_int(&exponential_backoff_strategy->base.ref_count, 1);
    exponential_backoff_strategy->config = *config;
    exponential_backoff_strategy->config.el_group =
        aws_event_loop_group_acquire(exponential_backoff_strategy->config.el_group);

    if (!exponential_backoff_strategy->config.generate_random &&
        !exponential_backoff_strategy->config.generate_random_impl) {
        exponential_backoff_strategy->config.generate_random_impl = s_default_gen_rand;
    }

    if (!exponential_backoff_strategy->config.max_retries) {
        exponential_backoff_strategy->config.max_retries = 5;
    }

    if (!exponential_backoff_strategy->config.backoff_scale_factor_ms) {
        exponential_backoff_strategy->config.backoff_scale_factor_ms = 500;
    }

    if (!exponential_backoff_strategy->config.max_backoff_secs) {
        exponential_backoff_strategy->config.max_backoff_secs = 20;
    }

    if (config->shutdown_options) {
        exponential_backoff_strategy->shutdown_options = *config->shutdown_options;
    }
    return &exponential_backoff_strategy->base;
}
