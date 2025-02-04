/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/log_formatter.h>

#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>

#include <inttypes.h>
#include <stdarg.h>

/*
 * Default formatter implementation
 */

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

enum {
    /* (max) strlen of "[<LogLevel>]" */
    LOG_LEVEL_PREFIX_PADDING = 7,

    /* (max) strlen of "[<ThreadId>]" */
    THREAD_ID_PREFIX_PADDING = 22,

    /* strlen of (user-content separator) " - " + "\n" + spaces between prefix fields + brackets around timestamp + 1 +
       subject_name padding */
    MISC_PADDING = 15,
};

#define MAX_LOG_LINE_PREFIX_SIZE                                                                                       \
    (LOG_LEVEL_PREFIX_PADDING + THREAD_ID_PREFIX_PADDING + MISC_PADDING + AWS_DATE_TIME_STR_MAX_LEN)

static size_t s_advance_and_clamp_index(size_t current_index, int amount, size_t maximum) {
    size_t next_index = current_index + amount;
    if (next_index > maximum) {
        next_index = maximum;
    }

    return next_index;
}

/* Thread-local string representation of current thread id */
AWS_THREAD_LOCAL struct {
    bool is_valid;
    char repr[AWS_THREAD_ID_T_REPR_BUFSZ];
} tl_logging_thread_id = {.is_valid = false};

int aws_format_standard_log_line(struct aws_logging_standard_formatting_data *formatting_data, va_list args) {
    size_t current_index = 0;

    /*
     * Begin the log line with "[<Log Level>] ["
     */
    const char *level_string = NULL;
    if (aws_log_level_to_string(formatting_data->level, &level_string)) {
        return AWS_OP_ERR;
    }

    if (formatting_data->total_length == 0) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /*
     * Use this length for all but the last write, so we guarantee room for the newline even if we get truncated
     */
    size_t fake_total_length = formatting_data->total_length - 1;

    int log_level_length = snprintf(formatting_data->log_line_buffer, fake_total_length, "[%s] [", level_string);
    if (log_level_length < 0) {
        return AWS_OP_ERR;
    }

    current_index = s_advance_and_clamp_index(current_index, log_level_length, fake_total_length);

    if (current_index < fake_total_length) {
        /*
         * Add the timestamp.  To avoid copies and allocations, do some byte buffer tomfoolery.
         *
         * First, make a byte_buf that points to the current position in the output string
         */
        struct aws_byte_buf timestamp_buffer = {
            .allocator = formatting_data->allocator,
            .buffer = (uint8_t *)formatting_data->log_line_buffer + current_index,
            .capacity = fake_total_length - current_index,
            .len = 0,
        };

        /*
         * Output the current time to the byte_buf
         */
        struct aws_date_time current_time;
        aws_date_time_init_now(&current_time);

        int result = aws_date_time_to_utc_time_str(&current_time, formatting_data->date_format, &timestamp_buffer);
        if (result != AWS_OP_SUCCESS) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        current_index = s_advance_and_clamp_index(current_index, (int)timestamp_buffer.len, fake_total_length);
    }

    if (current_index < fake_total_length) {
        /*
         * Add thread id and user content separator (" - ")
         */
        if (!tl_logging_thread_id.is_valid) {
            aws_thread_id_t current_thread_id = aws_thread_current_thread_id();
            if (aws_thread_id_t_to_string(current_thread_id, tl_logging_thread_id.repr, AWS_THREAD_ID_T_REPR_BUFSZ)) {
                return AWS_OP_ERR;
            }
            tl_logging_thread_id.is_valid = true;
        }
        int thread_id_written = snprintf(
            formatting_data->log_line_buffer + current_index,
            fake_total_length - current_index,
            "] [%s] ",
            tl_logging_thread_id.repr);
        if (thread_id_written < 0) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
        current_index = s_advance_and_clamp_index(current_index, thread_id_written, fake_total_length);
    }

    if (current_index < fake_total_length) {
        /* output subject name */
        if (formatting_data->subject_name) {
            int subject_written = snprintf(
                formatting_data->log_line_buffer + current_index,
                fake_total_length - current_index,
                "[%s]",
                formatting_data->subject_name);

            if (subject_written < 0) {
                return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            }

            current_index = s_advance_and_clamp_index(current_index, subject_written, fake_total_length);
        }
    }

    if (current_index < fake_total_length) {
        int separator_written =
            snprintf(formatting_data->log_line_buffer + current_index, fake_total_length - current_index, " - ");
        current_index = s_advance_and_clamp_index(current_index, separator_written, fake_total_length);
    }

    if (current_index < fake_total_length) {
        /*
         * Now write the actual data requested by the user
         */
#ifdef _WIN32
        int written_count = vsnprintf_s(
            formatting_data->log_line_buffer + current_index,
            fake_total_length - current_index,
            _TRUNCATE,
            formatting_data->format,
            args);
#else
        int written_count = vsnprintf(
            formatting_data->log_line_buffer + current_index,
            fake_total_length - current_index,
            formatting_data->format,
            args);
#endif /* _WIN32 */
        if (written_count < 0) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        current_index = s_advance_and_clamp_index(current_index, written_count, fake_total_length);
    }

    /*
     * End with a newline.
     */
    int newline_written_count =
        snprintf(formatting_data->log_line_buffer + current_index, formatting_data->total_length - current_index, "\n");
    if (newline_written_count < 0) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); /* we saved space, so this would be crazy */
    }

    formatting_data->amount_written = current_index + newline_written_count;

    return AWS_OP_SUCCESS;
}

struct aws_default_log_formatter_impl {
    enum aws_date_format date_format;
};

static int s_default_aws_log_formatter_format(
    struct aws_log_formatter *formatter,
    struct aws_string **formatted_output,
    enum aws_log_level level,
    aws_log_subject_t subject,
    const char *format,
    va_list args) {

    (void)subject;

    struct aws_default_log_formatter_impl *impl = formatter->impl;

    if (formatted_output == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /*
     * Calculate how much room we'll need to build the full log line.
     * You cannot consume a va_list twice, so we have to copy it.
     */
    va_list tmp_args;
    va_copy(tmp_args, args);
#ifdef _WIN32
    int required_length = _vscprintf(format, tmp_args) + 1;
#else
    int required_length = vsnprintf(NULL, 0, format, tmp_args) + 1;
#endif
    va_end(tmp_args);

    /*
     * Allocate enough room to hold the line.  Then we'll (unsafely) do formatted IO directly into the aws_string
     * memory.
     */
    const char *subject_name = aws_log_subject_name(subject);
    int subject_name_len = 0;

    if (subject_name) {
        subject_name_len = (int)strlen(subject_name);
    }

    int total_length = required_length + MAX_LOG_LINE_PREFIX_SIZE + subject_name_len;
    struct aws_string *raw_string = aws_mem_calloc(formatter->allocator, 1, sizeof(struct aws_string) + total_length);
    if (raw_string == NULL) {
        goto error_clean_up;
    }

    struct aws_logging_standard_formatting_data format_data = {
        .log_line_buffer = (char *)raw_string->bytes,
        .total_length = total_length,
        .level = level,
        .subject_name = subject_name,
        .format = format,
        .date_format = impl->date_format,
        .allocator = formatter->allocator,
        .amount_written = 0,
    };

    if (aws_format_standard_log_line(&format_data, args)) {
        goto error_clean_up;
    }

    *(struct aws_allocator **)(&raw_string->allocator) = formatter->allocator;
    *(size_t *)(&raw_string->len) = format_data.amount_written;

    *formatted_output = raw_string;

    return AWS_OP_SUCCESS;

error_clean_up:

    if (raw_string != NULL) {
        aws_mem_release(formatter->allocator, raw_string);
    }

    return AWS_OP_ERR;
}

static void s_default_aws_log_formatter_clean_up(struct aws_log_formatter *formatter) {
    aws_mem_release(formatter->allocator, formatter->impl);
}

static struct aws_log_formatter_vtable s_default_log_formatter_vtable = {
    .format = s_default_aws_log_formatter_format,
    .clean_up = s_default_aws_log_formatter_clean_up,
};

int aws_log_formatter_init_default(
    struct aws_log_formatter *formatter,
    struct aws_allocator *allocator,
    struct aws_log_formatter_standard_options *options) {
    struct aws_default_log_formatter_impl *impl =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_default_log_formatter_impl));
    impl->date_format = options->date_format;

    formatter->vtable = &s_default_log_formatter_vtable;
    formatter->allocator = allocator;
    formatter->impl = impl;

    return AWS_OP_SUCCESS;
}

void aws_log_formatter_clean_up(struct aws_log_formatter *formatter) {
    AWS_ASSERT(formatter->vtable->clean_up);
    (formatter->vtable->clean_up)(formatter);
}
