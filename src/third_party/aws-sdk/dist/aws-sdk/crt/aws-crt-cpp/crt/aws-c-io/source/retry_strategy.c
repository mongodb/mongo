/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/retry_strategy.h>

void aws_retry_strategy_acquire(struct aws_retry_strategy *retry_strategy) {
    size_t old_value = aws_atomic_fetch_add_explicit(&retry_strategy->ref_count, 1, aws_memory_order_relaxed);
    AWS_ASSERT(old_value > 0 && "aws_retry_strategy refcount had been zero, it's invalid to use it again.");
    (void)old_value;
}

void aws_retry_strategy_release(struct aws_retry_strategy *retry_strategy) {
    if (retry_strategy) {
        size_t old_value = aws_atomic_fetch_sub_explicit(&retry_strategy->ref_count, 1, aws_memory_order_seq_cst);
        AWS_ASSERT(old_value > 0 && "aws_retry_strategy refcount has gone negative");
        if (old_value == 1) {
            retry_strategy->vtable->destroy(retry_strategy);
        }
    }
}

int aws_retry_strategy_acquire_retry_token(
    struct aws_retry_strategy *retry_strategy,
    const struct aws_byte_cursor *partition_id,
    aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
    void *user_data,
    uint64_t timeout_ms) {
    AWS_PRECONDITION(retry_strategy);
    AWS_PRECONDITION(retry_strategy->vtable->acquire_token);
    return retry_strategy->vtable->acquire_token(retry_strategy, partition_id, on_acquired, user_data, timeout_ms);
}

int aws_retry_strategy_schedule_retry(
    struct aws_retry_token *token,
    enum aws_retry_error_type error_type,
    aws_retry_strategy_on_retry_ready_fn *retry_ready,
    void *user_data) {
    AWS_PRECONDITION(token);
    AWS_PRECONDITION(token->retry_strategy);
    AWS_PRECONDITION(token->retry_strategy->vtable->schedule_retry);

    return token->retry_strategy->vtable->schedule_retry(token, error_type, retry_ready, user_data);
}

int aws_retry_token_record_success(struct aws_retry_token *token) {
    AWS_PRECONDITION(token);
    AWS_PRECONDITION(token->retry_strategy);
    AWS_PRECONDITION(token->retry_strategy->vtable->record_success);

    return token->retry_strategy->vtable->record_success(token);
}

void aws_retry_token_acquire(struct aws_retry_token *token) {
    size_t old_value = aws_atomic_fetch_add_explicit(&token->ref_count, 1u, aws_memory_order_relaxed);
    AWS_ASSERT(old_value > 0 && "aws_retry_token refcount had been zero, it's invalid to use it again.");
    (void)old_value;
}

void aws_retry_token_release(struct aws_retry_token *token) {
    if (token) {
        AWS_PRECONDITION(token->retry_strategy);
        AWS_PRECONDITION(token->retry_strategy->vtable->release_token);

        size_t old_value = aws_atomic_fetch_sub_explicit(&token->ref_count, 1u, aws_memory_order_seq_cst);
        AWS_ASSERT(old_value > 0 && "aws_retry_token refcount has gone negative");
        if (old_value == 1u) {
            token->retry_strategy->vtable->release_token(token);
        }
    }
}
