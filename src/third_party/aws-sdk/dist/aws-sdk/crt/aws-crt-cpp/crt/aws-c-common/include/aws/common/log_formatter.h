
#ifndef AWS_COMMON_LOG_FORMATTER_H
#define AWS_COMMON_LOG_FORMATTER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

#include <aws/common/date_time.h>
#include <aws/common/logging.h>

#include <stdarg.h>
#include <stdio.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_allocator;
struct aws_string;

/*
 * Log formatter interface and default implementation
 *
 * Log formatters are invoked by the LOGF_* macros to transform a set of arguments into
 * one or more lines of text to be output to a logging sink (writer).
 */
struct aws_log_formatter;

typedef int(aws_log_formatter_format_fn)(
    struct aws_log_formatter *formatter,
    struct aws_string **formatted_output,
    enum aws_log_level level,
    aws_log_subject_t subject,
    const char *format,
    va_list args);

typedef void(aws_log_formatter_clean_up_fn)(struct aws_log_formatter *logger);

struct aws_log_formatter_vtable {
    aws_log_formatter_format_fn *format;
    aws_log_formatter_clean_up_fn *clean_up;
};

struct aws_log_formatter {
    struct aws_log_formatter_vtable *vtable;
    struct aws_allocator *allocator;
    void *impl;
};

struct aws_log_formatter_standard_options {
    enum aws_date_format date_format;
};

struct aws_logging_standard_formatting_data {
    char *log_line_buffer;
    size_t total_length;
    enum aws_log_level level;
    const char *subject_name;
    const char *format;
    enum aws_date_format date_format;
    struct aws_allocator *allocator; /* not used, just there to make byte_bufs valid */

    size_t amount_written;
};

AWS_EXTERN_C_BEGIN

/*
 * Initializes the default log formatter which outputs lines in the format:
 *
 *   [<LogLevel>] [<Timestamp>] [<ThreadId>] - <User content>\n
 */
AWS_COMMON_API
int aws_log_formatter_init_default(
    struct aws_log_formatter *formatter,
    struct aws_allocator *allocator,
    struct aws_log_formatter_standard_options *options);

/*
 * Cleans up a log formatter (minus the base structure memory) by calling the formatter's clean_up function
 * via the vtable.
 */
AWS_COMMON_API
void aws_log_formatter_clean_up(struct aws_log_formatter *formatter);

/*
 * Formats a single log line based on the input + the var args list.  Output is written to a fixed-size
 * buffer supplied in the data struct.
 */
AWS_COMMON_API
int aws_format_standard_log_line(struct aws_logging_standard_formatting_data *formatting_data, va_list args);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_LOG_FORMATTER_H */
