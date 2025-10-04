/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/logging.h>
#include <aws/io/retry_strategy.h>

#include <aws/common/byte_buf.h>
#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>

#include <inttypes.h>

AWS_STRING_FROM_LITERAL(s_empty_string, "");
static struct aws_byte_cursor s_empty_string_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("");
static const size_t s_initial_retry_bucket_capacity = 500u;
static const size_t s_standard_retry_cost = 5u;
static const size_t s_standard_transient_cost = 10u;
static const size_t s_standard_no_retry_cost = 1u;

struct retry_bucket {
    struct aws_allocator *allocator;
    struct aws_retry_strategy *owner;
    struct aws_string *partition_id;
    struct aws_byte_cursor partition_id_cur;
    struct {
        size_t current_capacity;
        struct aws_mutex partition_lock;
    } synced_data;
};

struct retry_bucket_token {
    struct aws_retry_token retry_token;
    struct retry_bucket *strategy_bucket;
    struct aws_retry_token *exp_backoff_token;
    aws_retry_strategy_on_retry_token_acquired_fn *original_on_acquired;
    aws_retry_strategy_on_retry_ready_fn *original_on_ready;
    size_t last_retry_cost;
    void *original_user_data;
};

static bool s_partition_id_equals_byte_cur(const void *seated_cur, const void *cur_ptr) {
    return aws_byte_cursor_eq_ignore_case(seated_cur, cur_ptr);
}

static uint64_t s_hash_partition_id(const void *seated_partition_ptr) {
    return aws_hash_byte_cursor_ptr_ignore_case(seated_partition_ptr);
}

static void s_destroy_standard_retry_bucket(void *retry_bucket) {
    struct retry_bucket *standard_retry_bucket = retry_bucket;
    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: destroying bucket partition " PRInSTR,
        (void *)standard_retry_bucket->owner,
        AWS_BYTE_CURSOR_PRI(standard_retry_bucket->partition_id_cur));
    aws_string_destroy(standard_retry_bucket->partition_id);
    aws_mutex_clean_up(&standard_retry_bucket->synced_data.partition_lock);
    aws_mem_release(standard_retry_bucket->allocator, standard_retry_bucket);
}

struct standard_strategy {
    struct aws_retry_strategy base;
    struct aws_retry_strategy *exponential_backoff_retry_strategy;
    size_t max_capacity;
    struct {
        struct aws_hash_table token_buckets;
        struct aws_mutex lock;
    } synced_data;
};

static void s_standard_retry_destroy(struct aws_retry_strategy *retry_strategy) {
    AWS_LOGF_TRACE(AWS_LS_IO_STANDARD_RETRY_STRATEGY, "id=%p: destroying self", (void *)retry_strategy);
    struct standard_strategy *standard_strategy = retry_strategy->impl;
    aws_retry_strategy_release(standard_strategy->exponential_backoff_retry_strategy);
    aws_hash_table_clean_up(&standard_strategy->synced_data.token_buckets);
    aws_mutex_clean_up(&standard_strategy->synced_data.lock);
    aws_mem_release(retry_strategy->allocator, standard_strategy);
}

static void s_on_standard_retry_token_acquired(
    struct aws_retry_strategy *retry_strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {
    (void)retry_strategy;
    (void)token;

    struct retry_bucket_token *retry_token = user_data;
    AWS_LOGF_DEBUG(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: token acquired callback invoked with error %s with token %p and nested token %p",
        (void *)retry_token->retry_token.retry_strategy,
        aws_error_str(error_code),
        (void *)&retry_token->retry_token,
        (void *)token);

    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: invoking on_retry_token_acquired callback",
        (void *)retry_token->retry_token.retry_strategy);

    aws_retry_token_acquire(&retry_token->retry_token);
    if (!error_code) {
        retry_token->exp_backoff_token = token;

        retry_token->original_on_acquired(
            retry_token->strategy_bucket->owner,
            error_code,
            &retry_token->retry_token,
            retry_token->original_user_data);
        AWS_LOGF_TRACE(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: on_retry_token_acquired callback completed",
            (void *)retry_token->retry_token.retry_strategy);

    } else {
        retry_token->original_on_acquired(
            retry_token->strategy_bucket->owner, error_code, NULL, retry_token->original_user_data);
        AWS_LOGF_TRACE(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: on_retry_token_acquired callback completed",
            (void *)retry_token->retry_token.retry_strategy);
    }
    aws_retry_token_release(&retry_token->retry_token);
}

static int s_standard_retry_acquire_token(
    struct aws_retry_strategy *retry_strategy,
    const struct aws_byte_cursor *partition_id,
    aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
    void *user_data,
    uint64_t timeout_ms) {
    struct standard_strategy *standard_strategy = retry_strategy->impl;
    bool bucket_needs_cleanup = false;

    const struct aws_byte_cursor *partition_id_ptr =
        !partition_id || partition_id->len == 0 ? &s_empty_string_cur : partition_id;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: attempting to acquire retry token for partition_id " PRInSTR,
        (void *)retry_strategy,
        AWS_BYTE_CURSOR_PRI(*partition_id_ptr));

    struct retry_bucket_token *token = aws_mem_calloc(retry_strategy->allocator, 1, sizeof(struct retry_bucket_token));
    if (!token) {
        return AWS_OP_ERR;
    }

    token->original_user_data = user_data;
    token->original_on_acquired = on_acquired;

    struct aws_hash_element *element_ptr;
    struct retry_bucket *bucket_ptr;
    AWS_FATAL_ASSERT(!aws_mutex_lock(&standard_strategy->synced_data.lock) && "Lock acquisition failed.");
    aws_hash_table_find(&standard_strategy->synced_data.token_buckets, partition_id_ptr, &element_ptr);
    if (!element_ptr) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: bucket for partition_id " PRInSTR " does not exist, attempting to create one",
            (void *)retry_strategy,
            AWS_BYTE_CURSOR_PRI(*partition_id_ptr));
        bucket_ptr = aws_mem_calloc(standard_strategy->base.allocator, 1, sizeof(struct retry_bucket));

        if (!bucket_ptr) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_STANDARD_RETRY_STRATEGY,
                "id=%p: error when allocating bucket %s",
                (void *)retry_strategy,
                aws_error_debug_str(aws_last_error()));
            goto table_locked;
        }

        bucket_needs_cleanup = true;
        bucket_ptr->allocator = standard_strategy->base.allocator;
        bucket_ptr->partition_id = partition_id_ptr->len > 0
                                       ? aws_string_new_from_cursor(standard_strategy->base.allocator, partition_id)
                                       : (struct aws_string *)s_empty_string;

        if (!bucket_ptr->partition_id) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_STANDARD_RETRY_STRATEGY,
                "id=%p: error when allocating partition_id %s",
                (void *)retry_strategy,
                aws_error_debug_str(aws_last_error()));
            goto table_locked;
        }

        bucket_ptr->partition_id_cur = aws_byte_cursor_from_string(bucket_ptr->partition_id);
        AWS_FATAL_ASSERT(!aws_mutex_init(&bucket_ptr->synced_data.partition_lock) && "mutex init failed!");
        bucket_ptr->owner = retry_strategy;
        bucket_ptr->synced_data.current_capacity = standard_strategy->max_capacity;
        AWS_LOGF_DEBUG(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: bucket %p for partition_id " PRInSTR " created",
            (void *)retry_strategy,
            (void *)bucket_ptr,
            AWS_BYTE_CURSOR_PRI(*partition_id_ptr));

        if (aws_hash_table_put(
                &standard_strategy->synced_data.token_buckets, &bucket_ptr->partition_id_cur, bucket_ptr, NULL)) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_STANDARD_RETRY_STRATEGY,
                "id=%p: error when putting bucket to token_bucket table %s",
                (void *)retry_strategy,
                aws_error_debug_str(aws_last_error()));
            goto table_locked;
        }
        bucket_needs_cleanup = false;
    } else {
        bucket_ptr = element_ptr->value;
        AWS_LOGF_DEBUG(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: bucket %p for partition_id " PRInSTR " found",
            (void *)retry_strategy,
            (void *)bucket_ptr,
            AWS_BYTE_CURSOR_PRI(*partition_id_ptr));
    }
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&standard_strategy->synced_data.lock) && "Mutex unlock failed");

    token->strategy_bucket = bucket_ptr;
    token->retry_token.retry_strategy = retry_strategy;
    aws_atomic_init_int(&token->retry_token.ref_count, 1u);
    aws_retry_strategy_acquire(retry_strategy);
    token->retry_token.allocator = retry_strategy->allocator;
    token->retry_token.impl = token;

    /* don't decrement the capacity counter, but add the retry payback, so making calls that succeed allows for a
     * gradual recovery of the bucket capacity. Otherwise, we'd never recover from an outage. */
    token->last_retry_cost = s_standard_no_retry_cost;

    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: allocated token %p for partition_id " PRInSTR,
        (void *)retry_strategy,
        (void *)&token->retry_token,
        AWS_BYTE_CURSOR_PRI(*partition_id_ptr));

    if (aws_retry_strategy_acquire_retry_token(
            standard_strategy->exponential_backoff_retry_strategy,
            partition_id_ptr,
            s_on_standard_retry_token_acquired,
            token,
            timeout_ms)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: error when acquiring retry token from backing retry strategy %p: %s",
            (void *)retry_strategy,
            (void *)standard_strategy->exponential_backoff_retry_strategy,
            aws_error_debug_str(aws_last_error()));
        goto table_updated;
    }

    return AWS_OP_SUCCESS;

table_updated:
    AWS_FATAL_ASSERT(!aws_mutex_lock(&standard_strategy->synced_data.lock) && "Mutex lock failed");
    aws_hash_table_remove(&standard_strategy->synced_data.token_buckets, &bucket_ptr->partition_id_cur, NULL, NULL);
    bucket_needs_cleanup = false;

table_locked:
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&standard_strategy->synced_data.lock) && "Mutex unlock failed");

    if (bucket_needs_cleanup) {
        s_destroy_standard_retry_bucket(bucket_ptr);
    }

    aws_retry_token_release(&token->retry_token);

    return AWS_OP_ERR;
}

void s_standard_retry_strategy_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    (void)token;

    struct aws_retry_token *standard_retry_token = user_data;
    struct retry_bucket_token *impl = standard_retry_token->impl;
    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: invoking on_retry_ready callback with error %s, token %p, and nested token %p",
        (void *)token->retry_strategy,
        aws_error_str(error_code),
        (void *)standard_retry_token,
        (void *)token);
    struct aws_retry_strategy *retry_strategy = token->retry_strategy;
    /* we already hold a reference count here due to the previous acquire before scheduling, so don't worry
     * about incrementing standard_retry_token here */
    impl->original_on_ready(standard_retry_token, error_code, impl->original_user_data);
    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY, "id=%p: on_retry_ready callback completed", (void *)retry_strategy);
    /* this is to release the acquire we did before scheduling the retry. Release it now. */
    aws_retry_token_release(standard_retry_token);
}

static int s_standard_retry_strategy_schedule_retry(
    struct aws_retry_token *token,
    enum aws_retry_error_type error_type,
    aws_retry_strategy_on_retry_ready_fn *retry_ready,
    void *user_data) {

    if (error_type == AWS_RETRY_ERROR_TYPE_CLIENT_ERROR) {
        return aws_raise_error(AWS_IO_RETRY_PERMISSION_DENIED);
    }

    struct retry_bucket_token *impl = token->impl;

    size_t capacity_consumed = 0;

    AWS_FATAL_ASSERT(!aws_mutex_lock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex lock failed");
    size_t current_capacity = impl->strategy_bucket->synced_data.current_capacity;
    if (current_capacity == 0) {
        AWS_FATAL_ASSERT(
            !aws_mutex_unlock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex unlock failed");
        AWS_LOGF_INFO(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "token_id=%p: requested to schedule retry but the bucket capacity is empty. Rejecting retry request.",
            (void *)token);
        return aws_raise_error(AWS_IO_RETRY_PERMISSION_DENIED);
    }

    if (error_type == AWS_RETRY_ERROR_TYPE_TRANSIENT) {
        capacity_consumed = aws_min_size(current_capacity, s_standard_transient_cost);
    } else {
        /* you may be looking for throttling, but if that happened, the service told us to slow down,
         * but is otherwise healthy. Pay a smaller penalty for those. */
        capacity_consumed = aws_min_size(current_capacity, s_standard_retry_cost);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "token_id=%p: reducing retry capacity by %zu from %zu and scheduling retry.",
        (void *)token,
        capacity_consumed,
        current_capacity);
    impl->original_user_data = user_data;
    impl->original_on_ready = retry_ready;

    size_t previous_cost = impl->last_retry_cost;
    impl->last_retry_cost = capacity_consumed;
    impl->strategy_bucket->synced_data.current_capacity -= capacity_consumed;
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex unlock failed");

    /* acquire before scheduling to prevent clean up before the callback runs. */
    aws_retry_token_acquire(&impl->retry_token);
    if (aws_retry_strategy_schedule_retry(
            impl->exp_backoff_token, error_type, s_standard_retry_strategy_on_retry_ready, token)) {
        /* release for the above acquire */
        aws_retry_token_release(&impl->retry_token);
        AWS_LOGF_ERROR(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "token_id=%p: error occurred while scheduling retry: %s.",
            (void *)token,
            aws_error_debug_str(aws_last_error()));
        /* roll it back. */
        AWS_FATAL_ASSERT(!aws_mutex_lock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex lock failed");
        impl->last_retry_cost = previous_cost;
        size_t desired_capacity = impl->strategy_bucket->synced_data.current_capacity + capacity_consumed;
        struct standard_strategy *strategy_impl = token->retry_strategy->impl;
        impl->strategy_bucket->synced_data.current_capacity =
            desired_capacity < strategy_impl->max_capacity ? desired_capacity : strategy_impl->max_capacity;
        AWS_FATAL_ASSERT(
            !aws_mutex_unlock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex unlock failed");

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_standard_retry_strategy_record_success(struct aws_retry_token *token) {
    struct retry_bucket_token *impl = token->impl;

    AWS_FATAL_ASSERT(!aws_mutex_lock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex lock failed");
    AWS_LOGF_DEBUG(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "token_id=%p: partition=" PRInSTR
        ": recording successful operation and adding %zu units of capacity back to the bucket.",
        (void *)token,
        AWS_BYTE_CURSOR_PRI(impl->strategy_bucket->partition_id_cur),
        impl->last_retry_cost);
    size_t capacity_payback = impl->strategy_bucket->synced_data.current_capacity + impl->last_retry_cost;
    struct standard_strategy *standard_strategy = token->retry_strategy->impl;
    impl->strategy_bucket->synced_data.current_capacity =
        capacity_payback < standard_strategy->max_capacity ? capacity_payback : standard_strategy->max_capacity;
    impl->last_retry_cost = 0;
    AWS_LOGF_TRACE(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "bucket_id=%p: partition=" PRInSTR " : new capacity is %zu.",
        (void *)token,
        AWS_BYTE_CURSOR_PRI(impl->strategy_bucket->partition_id_cur),
        impl->strategy_bucket->synced_data.current_capacity);
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&impl->strategy_bucket->synced_data.partition_lock) && "mutex unlock failed");
    return AWS_OP_SUCCESS;
}

static void s_standard_retry_strategy_release_token(struct aws_retry_token *token) {
    if (token) {
        AWS_LOGF_TRACE(AWS_LS_IO_STANDARD_RETRY_STRATEGY, "id=%p: releasing token", (void *)token);
        struct retry_bucket_token *impl = token->impl;
        aws_retry_token_release(impl->exp_backoff_token);
        aws_retry_strategy_release(token->retry_strategy);
        aws_mem_release(token->allocator, impl);
    }
}

static struct aws_retry_strategy_vtable s_standard_retry_vtable = {
    .schedule_retry = s_standard_retry_strategy_schedule_retry,
    .acquire_token = s_standard_retry_acquire_token,
    .release_token = s_standard_retry_strategy_release_token,
    .destroy = s_standard_retry_destroy,
    .record_success = s_standard_retry_strategy_record_success,
};

struct aws_retry_strategy *aws_retry_strategy_new_standard(
    struct aws_allocator *allocator,
    const struct aws_standard_retry_options *config) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(config);

    AWS_LOGF_INFO(AWS_LS_IO_STANDARD_RETRY_STRATEGY, "static: creating new standard retry strategy");
    struct standard_strategy *standard_strategy = aws_mem_calloc(allocator, 1, sizeof(struct standard_strategy));

    if (!standard_strategy) {
        AWS_LOGF_ERROR(AWS_LS_IO_STANDARD_RETRY_STRATEGY, "static: allocation of new standard retry strategy failed");
        return NULL;
    }

    aws_atomic_init_int(&standard_strategy->base.ref_count, 1);

    struct aws_exponential_backoff_retry_options config_cpy = config->backoff_retry_options;

    /* standard default is 3. */
    if (!config->backoff_retry_options.max_retries) {
        config_cpy.max_retries = 3;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: creating backing exponential backoff strategy with max_retries of %zu",
        (void *)&standard_strategy->base,
        config_cpy.max_retries);

    standard_strategy->exponential_backoff_retry_strategy =
        aws_retry_strategy_new_exponential_backoff(allocator, &config_cpy);

    if (!standard_strategy->exponential_backoff_retry_strategy) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: allocation of new exponential backoff retry strategy failed: %s",
            (void *)&standard_strategy->base,
            aws_error_debug_str(aws_last_error()));
        goto error;
    }

    if (aws_hash_table_init(
            &standard_strategy->synced_data.token_buckets,
            allocator,
            16u,
            s_hash_partition_id,
            s_partition_id_equals_byte_cur,
            NULL,
            s_destroy_standard_retry_bucket)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_STANDARD_RETRY_STRATEGY,
            "id=%p: token bucket table creation failed: %s",
            (void *)&standard_strategy->base,
            aws_error_debug_str(aws_last_error()));
        goto error;
    }

    standard_strategy->max_capacity =
        config->initial_bucket_capacity ? config->initial_bucket_capacity : s_initial_retry_bucket_capacity;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "id=%p: maximum bucket capacity set to %zu",
        (void *)&standard_strategy->base,
        standard_strategy->max_capacity);
    AWS_FATAL_ASSERT(!aws_mutex_init(&standard_strategy->synced_data.lock) && "mutex init failed");

    standard_strategy->base.allocator = allocator;
    standard_strategy->base.vtable = &s_standard_retry_vtable;
    standard_strategy->base.impl = standard_strategy;
    return &standard_strategy->base;

error:
    if (standard_strategy->exponential_backoff_retry_strategy) {
        aws_retry_strategy_release(standard_strategy->exponential_backoff_retry_strategy);
    }

    aws_mem_release(allocator, standard_strategy);

    return NULL;
}
