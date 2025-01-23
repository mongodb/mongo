/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_ENDPOINTS_REGEX_H
#define AWS_SDKUTILS_ENDPOINTS_REGEX_H

#include <aws/common/byte_buf.h>
#include <aws/sdkutils/sdkutils.h>

/*
 * Warning: this is a minimal regex implementation that only covers cases needed
 * for endpoint resolution and it is missing a lot of regex features.
 * Please reconsider if you are planning to use below functions in any other
 * context than endpoint resolution.
 * Refer to source/endpoints_regex.c for limitations.
 */

struct aws_endpoints_regex;

/*
 * Parse regex pattern and construct "compiled" regex from it.
 * Returns NULL on failure and raises following error code:
 * - AWS_ERROR_INVALID_ARGUMENT - regex is invalid for some reason
 * - AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX - regex is valid, but
 *   implementation does not support some of regex features
 */
AWS_SDKUTILS_API struct aws_endpoints_regex *aws_endpoints_regex_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor regex_pattern);

/*
 * Destroys compiled regex.
 */
AWS_SDKUTILS_API void aws_endpoints_regex_destroy(struct aws_endpoints_regex *regex);

/*
 * Matches text against regex.
 * returns AWS_OP_SUCCESS on successful match and
 * AWS_ERROR_SDKUTILS_ENDPOINTS_REGEX_NO_MATCH if text didn't match or
 * AWS_ERROR_INVALID_ARGUMENT if inputs are invalid.
 *
 */
AWS_SDKUTILS_API int aws_endpoints_regex_match(const struct aws_endpoints_regex *regex, struct aws_byte_cursor text);

#endif /* AWS_SDKUTILS_ENDPOINTS_REGEX_H */
