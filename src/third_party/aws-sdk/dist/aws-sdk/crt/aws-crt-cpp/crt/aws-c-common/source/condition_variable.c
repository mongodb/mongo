/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/condition_variable.h>

int aws_condition_variable_wait_pred(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    aws_condition_predicate_fn *pred,
    void *pred_ctx) {

    int err_code = 0;
    while (!err_code && !pred(pred_ctx)) {
        err_code = aws_condition_variable_wait(condition_variable, mutex);
    }

    return err_code;
}

int aws_condition_variable_wait_for_pred(
    struct aws_condition_variable *condition_variable,
    struct aws_mutex *mutex,
    int64_t time_to_wait,
    aws_condition_predicate_fn *pred,
    void *pred_ctx) {

    int err_code = 0;
    while (!err_code && !pred(pred_ctx)) {
        err_code = aws_condition_variable_wait_for(condition_variable, mutex, time_to_wait);
    }

    return err_code;
}
