/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/environment.h>

#include <aws/common/string.h>
#include <stdlib.h>

int aws_get_environment_value(
    struct aws_allocator *allocator,
    const struct aws_string *variable_name,
    struct aws_string **value_out) {

    const char *value = getenv(aws_string_c_str(variable_name));
    if (value == NULL) {
        *value_out = NULL;
        return AWS_OP_SUCCESS;
    }

    *value_out = aws_string_new_from_c_str(allocator, value);
    if (*value_out == NULL) {
        return aws_raise_error(AWS_ERROR_ENVIRONMENT_GET);
    }

    return AWS_OP_SUCCESS;
}

int aws_set_environment_value(const struct aws_string *variable_name, const struct aws_string *value) {

    if (setenv(aws_string_c_str(variable_name), aws_string_c_str(value), 1) != 0) {
        return aws_raise_error(AWS_ERROR_ENVIRONMENT_SET);
    }

    return AWS_OP_SUCCESS;
}

int aws_unset_environment_value(const struct aws_string *variable_name) {
    if (unsetenv(aws_string_c_str(variable_name)) != 0) {
        return aws_raise_error(AWS_ERROR_ENVIRONMENT_UNSET);
    }

    return AWS_OP_SUCCESS;
}
