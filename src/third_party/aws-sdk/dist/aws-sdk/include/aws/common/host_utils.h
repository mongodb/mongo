#ifndef AWS_COMMON_HOST_UTILS_H
#define AWS_COMMON_HOST_UTILS_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

struct aws_byte_cursor;

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/*
 * Determine whether host cursor is IPv4 string.
 */
AWS_COMMON_API bool aws_host_utils_is_ipv4(struct aws_byte_cursor host);

/*
 * Determine whether host cursor is IPv6 string.
 * Supports checking for uri encoded strings and scoped literals.
 */
AWS_COMMON_API bool aws_host_utils_is_ipv6(struct aws_byte_cursor host, bool is_uri_encoded);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_HOST_UTILS_H */
