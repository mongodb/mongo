/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/process.h>
#include <aws/common/string.h>

#include <stdio.h>
#include <sys/types.h>

enum { MAX_BUFFER_SIZE = 2048 };

int aws_run_command_result_init(struct aws_allocator *allocator, struct aws_run_command_result *result) {
    if (!allocator || !result) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    AWS_ZERO_STRUCT(*result);
    return AWS_OP_SUCCESS;
}

void aws_run_command_result_cleanup(struct aws_run_command_result *result) {
    if (!result) {
        return;
    }
    aws_string_destroy_secure(result->std_out);
    aws_string_destroy_secure(result->std_err);
}

#if defined(AWS_OS_WINDOWS) && !defined(AWS_OS_WINDOWS_DESKTOP)
int aws_run_command(
    struct aws_allocator *allocator,
    struct aws_run_command_options *options,
    struct aws_run_command_result *result) {
    (void)allocator;
    (void)options;
    (void)result;
    return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
}
#else

int aws_run_command(
    struct aws_allocator *allocator,
    struct aws_run_command_options *options,
    struct aws_run_command_result *result) {

    AWS_FATAL_ASSERT(allocator);
    AWS_FATAL_ASSERT(options);
    AWS_FATAL_ASSERT(result);

    FILE *output_stream;
    char output_buffer[MAX_BUFFER_SIZE];
    struct aws_byte_buf result_buffer;
    int ret = AWS_OP_ERR;
    if (aws_byte_buf_init(&result_buffer, allocator, MAX_BUFFER_SIZE)) {
        goto on_finish;
    }

#    if defined(AWS_OS_WINDOWS)
    output_stream = _popen(options->command, "r");
#    else
    output_stream = popen(options->command, "r");
#    endif

    if (output_stream) {
        while (!feof(output_stream)) {
            if (fgets(output_buffer, MAX_BUFFER_SIZE, output_stream) != NULL) {
                struct aws_byte_cursor cursor = aws_byte_cursor_from_c_str(output_buffer);
                if (aws_byte_buf_append_dynamic(&result_buffer, &cursor)) {
                    goto on_finish;
                }
            }
        }
#    if defined(AWS_OS_WINDOWS)
        result->ret_code = _pclose(output_stream);
#    else
        result->ret_code = pclose(output_stream);
#    endif
    }

    struct aws_byte_cursor trim_cursor = aws_byte_cursor_from_buf(&result_buffer);
    struct aws_byte_cursor trimmed_cursor = aws_byte_cursor_trim_pred(&trim_cursor, aws_char_is_space);
    if (trimmed_cursor.len) {
        result->std_out = aws_string_new_from_array(allocator, trimmed_cursor.ptr, trimmed_cursor.len);
        if (!result->std_out) {
            goto on_finish;
        }
    }
    ret = AWS_OP_SUCCESS;

on_finish:
    aws_byte_buf_clean_up_secure(&result_buffer);
    return ret;
}
#endif /* !AWS_OS_WINDOWS */
