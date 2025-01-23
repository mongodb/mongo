#ifndef AWS_COMMON_CONDITION_VARIABLE_H
#define AWS_COMMON_CONDITION_VARIABLE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#ifndef _WIN32
#    include <pthread.h>
#endif

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_mutex;

struct aws_condition_variable;

typedef bool(aws_condition_predicate_fn)(void *);

struct aws_condition_variable {
#ifdef _WIN32
    void *condition_handle;
#else
    pthread_cond_t condition_handle;
#endif
    bool initialized;
};

/**
 * Static initializer for condition variable.
 * You can do something like struct aws_condition_variable var =
 * AWS_CONDITION_VARIABLE_INIT;
 *
 * If on Windows and you get an error about AWS_CONDITION_VARIABLE_INIT being undefined, please include windows.h to get
 * CONDITION_VARIABLE_INIT.
 */
#ifdef _WIN32
#    define AWS_CONDITION_VARIABLE_INIT {.condition_handle = NULL, .initialized = true}
#else
#    define AWS_CONDITION_VARIABLE_INIT {.condition_handle = PTHREAD_COND_INITIALIZER, .initialized = true}
#endif

AWS_EXTERN_C_BEGIN

/**
 * Initializes a condition variable.
 */
AWS_COMMON_API
int aws_condition_variable_init(struct aws_condition_variable *condition_variable);

/**
 * Cleans up a condition variable.
 */
AWS_COMMON_API
void aws_condition_variable_clean_up(struct aws_condition_variable *condition_variable);

/**
 * Notifies/Wakes one waiting thread
 */
AWS_COMMON_API
int aws_condition_variable_notify_one(struct aws_condition_variable *condition_variable);

/**
 * Notifies/Wakes all waiting threads.
 */
AWS_COMMON_API
int aws_condition_variable_notify_all(struct aws_condition_variable *condition_variable);

/**
 * Waits the calling thread on a notification from another thread. This function must be called with the mutex locked
 * by the calling thread otherwise the behavior is undefined. Spurious wakeups can occur and to avoid this causing
 * any problems use the _pred version of this function.
 */
AWS_COMMON_API
int aws_condition_variable_wait(struct aws_condition_variable *condition_variable, struct aws_mutex *mutex);

/**
 * Waits the calling thread on a notification from another thread. If predicate returns false, the wait is reentered,
 * otherwise control returns to the caller. This function must be called with the mutex locked by the calling thread
 * otherwise the behavior is undefined.
 */
AWS_COMMON_API
int aws_condition_variable_wait_pred(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    aws_condition_predicate_fn *pred,
    void *pred_ctx);

/**
 * Waits the calling thread on a notification from another thread. Times out after time_to_wait. time_to_wait is in
 * nanoseconds. This function must be called with the mutex locked by the calling thread otherwise the behavior is
 * undefined. Spurious wakeups can occur and to avoid this causing any problems use the _pred version of this function.
 */
AWS_COMMON_API
int aws_condition_variable_wait_for(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    int64_t time_to_wait);

/**
 * Waits the calling thread on a notification from another thread. Times out after time_to_wait. time_to_wait is in
 * nanoseconds. If predicate returns false, the wait is reentered, otherwise control returns to the caller. This
 * function must be called with the mutex locked by the calling thread otherwise the behavior is undefined.
 */
AWS_COMMON_API
int aws_condition_variable_wait_for_pred(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    int64_t time_to_wait,
    aws_condition_predicate_fn *pred,
    void *pred_ctx);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_CONDITION_VARIABLE_H */
