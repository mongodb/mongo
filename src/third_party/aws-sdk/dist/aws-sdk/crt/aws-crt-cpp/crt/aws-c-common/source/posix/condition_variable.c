/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/condition_variable.h>

#include <aws/common/clock.h>
#include <aws/common/mutex.h>

#include <errno.h>

static int process_error_code(int err) {
    switch (err) {
        case ENOMEM:
            return aws_raise_error(AWS_ERROR_OOM);
        case ETIMEDOUT:
            return aws_raise_error(AWS_ERROR_COND_VARIABLE_TIMED_OUT);
        default:
            return aws_raise_error(AWS_ERROR_COND_VARIABLE_ERROR_UNKNOWN);
    }
}

int aws_condition_variable_init(struct aws_condition_variable *condition_variable) {
    AWS_PRECONDITION(condition_variable);

    if (pthread_cond_init(&condition_variable->condition_handle, NULL)) {
        AWS_ZERO_STRUCT(*condition_variable);
        return aws_raise_error(AWS_ERROR_COND_VARIABLE_INIT_FAILED);
    }

    condition_variable->initialized = true;
    return AWS_OP_SUCCESS;
}

void aws_condition_variable_clean_up(struct aws_condition_variable *condition_variable) {
    AWS_PRECONDITION(condition_variable);

    if (condition_variable->initialized) {
        pthread_cond_destroy(&condition_variable->condition_handle);
    }

    AWS_ZERO_STRUCT(*condition_variable);
}

int aws_condition_variable_notify_one(struct aws_condition_variable *condition_variable) {
    AWS_PRECONDITION(condition_variable && condition_variable->initialized);

    int err_code = pthread_cond_signal(&condition_variable->condition_handle);

    if (err_code) {
        return process_error_code(err_code);
    }

    return AWS_OP_SUCCESS;
}

int aws_condition_variable_notify_all(struct aws_condition_variable *condition_variable) {
    AWS_PRECONDITION(condition_variable && condition_variable->initialized);

    int err_code = pthread_cond_broadcast(&condition_variable->condition_handle);

    if (err_code) {
        return process_error_code(err_code);
    }

    return AWS_OP_SUCCESS;
}

int aws_condition_variable_wait(struct aws_condition_variable *condition_variable, struct aws_mutex *mutex) {
    AWS_PRECONDITION(condition_variable && condition_variable->initialized);
    AWS_PRECONDITION(mutex && mutex->initialized);

    int err_code = pthread_cond_wait(&condition_variable->condition_handle, &mutex->mutex_handle);

    if (err_code) {
        return process_error_code(err_code);
    }

    return AWS_OP_SUCCESS;
}

int aws_condition_variable_wait_for(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    int64_t time_to_wait) {

    AWS_PRECONDITION(condition_variable && condition_variable->initialized);
    AWS_PRECONDITION(mutex && mutex->initialized);

    uint64_t current_sys_time = 0;
    if (aws_sys_clock_get_ticks(&current_sys_time)) {
        return AWS_OP_ERR;
    }

    struct timespec ts;
    uint64_t remainder = 0;
    ts.tv_sec = (time_t)aws_timestamp_convert(
        (uint64_t)(time_to_wait + current_sys_time), AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, &remainder);
    ts.tv_nsec = (long)remainder;

    int err_code = pthread_cond_timedwait(&condition_variable->condition_handle, &mutex->mutex_handle, &ts);

    if (err_code) {
        return process_error_code(err_code);
    }

    return AWS_OP_SUCCESS;
}
