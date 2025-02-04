/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/sdkutils/resource_name.h>

#define ARN_SPLIT_COUNT ((size_t)5)
#define ARN_PARTS_COUNT ((size_t)6)

static const char ARN_DELIMETER[] = ":";
static const char ARN_DELIMETER_CHAR = ':';

static const size_t DELIMETER_LEN = 8; /* strlen("arn:::::") */

int aws_resource_name_init_from_cur(struct aws_resource_name *arn, const struct aws_byte_cursor *input) {
    struct aws_byte_cursor arn_parts[ARN_PARTS_COUNT];
    struct aws_array_list arn_part_list;
    aws_array_list_init_static(&arn_part_list, arn_parts, ARN_PARTS_COUNT, sizeof(struct aws_byte_cursor));
    if (aws_byte_cursor_split_on_char_n(input, ARN_DELIMETER_CHAR, ARN_SPLIT_COUNT, &arn_part_list)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    struct aws_byte_cursor *arn_prefix;
    if (aws_array_list_get_at_ptr(&arn_part_list, (void **)&arn_prefix, 0) ||
        !aws_byte_cursor_eq_c_str(arn_prefix, "arn")) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    if (aws_array_list_get_at(&arn_part_list, &arn->partition, 1)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    if (aws_array_list_get_at(&arn_part_list, &arn->service, 2)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    if (aws_array_list_get_at(&arn_part_list, &arn->region, 3)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    if (aws_array_list_get_at(&arn_part_list, &arn->account_id, 4)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    if (aws_array_list_get_at(&arn_part_list, &arn->resource_id, 5)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }
    return AWS_OP_SUCCESS;
}

int aws_resource_name_length(const struct aws_resource_name *arn, size_t *size) {
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->partition));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->service));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->region));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->account_id));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->resource_id));

    *size = arn->partition.len + arn->region.len + arn->service.len + arn->account_id.len + arn->resource_id.len +
            DELIMETER_LEN;

    return AWS_OP_SUCCESS;
}

int aws_byte_buf_append_resource_name(struct aws_byte_buf *buf, const struct aws_resource_name *arn) {
    AWS_PRECONDITION(aws_byte_buf_is_valid(buf));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->partition));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->service));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->region));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->account_id));
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&arn->resource_id));

    const struct aws_byte_cursor prefix = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("arn:");
    const struct aws_byte_cursor colon_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(ARN_DELIMETER);

    if (aws_byte_buf_append(buf, &prefix)) {
        return aws_raise_error(aws_last_error());
    }
    if (aws_byte_buf_append(buf, &arn->partition)) {
        return aws_raise_error(aws_last_error());
    }
    if (aws_byte_buf_append(buf, &colon_cur)) {
        return aws_raise_error(aws_last_error());
    }

    if (aws_byte_buf_append(buf, &arn->service)) {
        return aws_raise_error(aws_last_error());
    }
    if (aws_byte_buf_append(buf, &colon_cur)) {
        return aws_raise_error(aws_last_error());
    }

    if (aws_byte_buf_append(buf, &arn->region)) {
        return aws_raise_error(aws_last_error());
    }
    if (aws_byte_buf_append(buf, &colon_cur)) {
        return aws_raise_error(aws_last_error());
    }

    if (aws_byte_buf_append(buf, &arn->account_id)) {
        return aws_raise_error(aws_last_error());
    }
    if (aws_byte_buf_append(buf, &colon_cur)) {
        return aws_raise_error(aws_last_error());
    }

    if (aws_byte_buf_append(buf, &arn->resource_id)) {
        return aws_raise_error(aws_last_error());
    }

    AWS_POSTCONDITION(aws_byte_buf_is_valid(buf));
    return AWS_OP_SUCCESS;
}
