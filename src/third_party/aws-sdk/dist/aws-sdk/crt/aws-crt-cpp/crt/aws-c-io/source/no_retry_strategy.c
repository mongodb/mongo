/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/ref_count.h>
#include <aws/io/io.h>
#include <aws/io/retry_strategy.h>

struct aws_retry_strategy_no_retries {
    struct aws_retry_strategy base;
    struct aws_shutdown_callback_options shutdown_options;
};

static void s_no_retry_destroy(struct aws_retry_strategy *retry_strategy) {
    if (retry_strategy) {
        struct aws_retry_strategy_no_retries *strategy = retry_strategy->impl;
        aws_simple_completion_callback *completion_callback = strategy->shutdown_options.shutdown_callback_fn;
        void *completion_user_data = strategy->shutdown_options.shutdown_callback_user_data;

        aws_mem_release(retry_strategy->allocator, strategy);
        if (completion_callback != NULL) {
            completion_callback(completion_user_data);
        }
    }
}

static int s_no_retry_acquire_token(
    struct aws_retry_strategy *retry_strategy,
    const struct aws_byte_cursor *partition_id,
    aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
    void *user_data,
    uint64_t timeout_ms) {
    (void)retry_strategy;
    (void)partition_id;
    (void)on_acquired;
    (void)user_data;
    (void)timeout_ms;
    return aws_raise_error(AWS_IO_RETRY_PERMISSION_DENIED);
}

static int s_no_retry_schedule_retry(
    struct aws_retry_token *token,
    enum aws_retry_error_type error_type,
    aws_retry_strategy_on_retry_ready_fn *retry_ready,
    void *user_data) {
    (void)token;
    (void)error_type;
    (void)retry_ready;
    (void)user_data;
    AWS_FATAL_ASSERT(0 && "schedule_retry must not be called for no_retries retry strategy");
}

static int s_no_retry_record_success(struct aws_retry_token *token) {
    (void)token;
    AWS_FATAL_ASSERT(0 && "record_success must not be called for no_retries retry strategy");
}

static void s_no_retry_release_token(struct aws_retry_token *token) {
    (void)token;
    AWS_FATAL_ASSERT(0 && "release_token must not be called for no_retries retry strategy");
}

static struct aws_retry_strategy_vtable s_exponential_retry_vtable = {
    .destroy = s_no_retry_destroy,
    .acquire_token = s_no_retry_acquire_token,
    .schedule_retry = s_no_retry_schedule_retry,
    .record_success = s_no_retry_record_success,
    .release_token = s_no_retry_release_token,
};

struct aws_retry_strategy *aws_retry_strategy_new_no_retry(
    struct aws_allocator *allocator,
    const struct aws_no_retry_options *config) {
    struct aws_retry_strategy_no_retries *strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_retry_strategy_no_retries));
    strategy->base.allocator = allocator;
    strategy->base.impl = strategy;
    strategy->base.vtable = &s_exponential_retry_vtable;
    aws_atomic_init_int(&strategy->base.ref_count, 1);

    if (config != NULL && config->shutdown_options) {
        strategy->shutdown_options = *config->shutdown_options;
    }
    return &strategy->base;
}
