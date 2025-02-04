/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/compression/compression.h>

#define DEFINE_ERROR_INFO(CODE, STR)                                                                                   \
    [(CODE) - AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_COMPRESSION_PACKAGE_ID)] =                                              \
        AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-c-compression")

/* clang-format off */
static struct aws_error_info s_errors[] = {
    DEFINE_ERROR_INFO(
        AWS_ERROR_COMPRESSION_UNKNOWN_SYMBOL,
        "Compression encountered an unknown symbol."),
};
/* clang-format on */

static struct aws_error_info_list s_error_list = {
    .error_list = s_errors,
    .count = AWS_ARRAY_SIZE(s_errors),
};

static bool s_library_initialized = false;
void aws_compression_library_init(struct aws_allocator *alloc) {
    if (s_library_initialized) {
        return;
    }
    s_library_initialized = true;

    aws_common_library_init(alloc);
    aws_register_error_info(&s_error_list);
}

void aws_compression_library_clean_up(void) {
    if (!s_library_initialized) {
        return;
    }
    s_library_initialized = false;

    aws_unregister_error_info(&s_error_list);
    aws_common_library_clean_up();
}
