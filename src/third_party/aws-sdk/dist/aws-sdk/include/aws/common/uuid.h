#ifndef AWS_COMMON_UUID_H
#define AWS_COMMON_UUID_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_byte_cursor;
struct aws_byte_buf;

struct aws_uuid {
    uint8_t uuid_data[16];
};

/* 36 bytes for the UUID plus one more for the null terminator. */
enum { AWS_UUID_STR_LEN = 37 };

AWS_EXTERN_C_BEGIN

AWS_COMMON_API int aws_uuid_init(struct aws_uuid *uuid);
AWS_COMMON_API int aws_uuid_init_from_str(struct aws_uuid *uuid, const struct aws_byte_cursor *uuid_str);
AWS_COMMON_API int aws_uuid_to_str(const struct aws_uuid *uuid, struct aws_byte_buf *output);
AWS_COMMON_API bool aws_uuid_equals(const struct aws_uuid *a, const struct aws_uuid *b);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_UUID_H */
