/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/event-stream/event_stream.h>

#include <aws/checksums/crc.h>

#include <aws/common/byte_buf.h>
#include <aws/common/encoding.h>
#include <aws/io/io.h>

#include <inttypes.h>

#define LIB_NAME "libaws-c-event-stream"

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4221) /* aggregate initializer using local variable addresses */
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#    pragma warning(disable : 4306) /* msft doesn't trust us to do pointer arithmetic. */
#endif

static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO(AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH, "Buffer length mismatch", LIB_NAME),
    AWS_DEFINE_ERROR_INFO(AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN, "insufficient buffer length", LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED,
        "a field for the message was too large",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE, "prelude checksum was incorrect", LIB_NAME),
    AWS_DEFINE_ERROR_INFO(AWS_ERROR_EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE, "message checksum was incorrect", LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN,
        "message headers length was incorrect",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_MESSAGE_UNKNOWN_HEADER_TYPE,
        "An unknown header type was encountered",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_MESSAGE_PARSER_ILLEGAL_STATE,
        "message parser encountered an illegal state",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_RPC_CONNECTION_CLOSED,
        "event stream rpc connection has been closed",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR,
        "event stream rpc connection has encountered a protocol error",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_RPC_STREAM_CLOSED,
        "event stream rpc connection stream is closed.",
        LIB_NAME),
    AWS_DEFINE_ERROR_INFO(
        AWS_ERROR_EVENT_STREAM_RPC_STREAM_NOT_ACTIVATED,
        "event stream rpc stream continuation was not successfully activated before use. Call "
        "aws_event_stream_rpc_client_continuation_activate()"
        " before using a stream continuation token.",
        LIB_NAME),
};

static struct aws_error_info_list s_list = {
    .error_list = s_errors,
    .count = sizeof(s_errors) / sizeof(struct aws_error_info),
};

static bool s_event_stream_library_initialized = false;

static struct aws_log_subject_info s_event_stream_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_EVENT_STREAM_GENERAL,
        "event-stream-general",
        "Subject for aws-c-event-stream logging that defies categorization."),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "event-stream-channel-handler",
        "Subject for event-stream channel handler related logging."),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_EVENT_STREAM_RPC_SERVER,
        "event-stream-rpc-server",
        "Subject for event-stream rpc server."),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_EVENT_STREAM_RPC_CLIENT,
        "event-stream-rpc-client",
        "Subject for event-stream rpc client."),
};

static struct aws_log_subject_info_list s_event_stream_log_subject_list = {
    .subject_list = s_event_stream_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_event_stream_log_subject_infos),
};

static const uint16_t UUID_LEN = 16U;

void aws_event_stream_library_init(struct aws_allocator *allocator) {
    if (!s_event_stream_library_initialized) {
        s_event_stream_library_initialized = true;
        aws_io_library_init(allocator);
        aws_register_error_info(&s_list);
        aws_register_log_subject_info_list(&s_event_stream_log_subject_list);
    }
}

void aws_event_stream_library_clean_up(void) {
    if (s_event_stream_library_initialized) {
        s_event_stream_library_initialized = false;
        aws_unregister_error_info(&s_list);
        aws_io_library_clean_up();
    }
}

#define TOTAL_LEN_OFFSET 0
#define PRELUDE_CRC_OFFSET (sizeof(uint32_t) + sizeof(uint32_t))
#define HEADER_LEN_OFFSET sizeof(uint32_t)

/* Computes the byte length necessary to store the headers represented in the headers list.
 * returns that length. */
uint32_t aws_event_stream_compute_headers_required_buffer_len(const struct aws_array_list *headers) {
    if (!headers || !aws_array_list_length(headers)) {
        return 0;
    }

    size_t headers_count = aws_array_list_length(headers);
    size_t headers_len = 0;

    for (size_t i = 0; i < headers_count; ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;

        aws_array_list_get_at_ptr(headers, (void **)&header, i);
        AWS_FATAL_ASSERT(
            !aws_add_size_checked(headers_len, sizeof(header->header_name_len), &headers_len) &&
            "integer overflow occurred computing total headers length.");
        AWS_FATAL_ASSERT(
            !aws_add_size_checked(headers_len, header->header_name_len + 1, &headers_len) &&
            "integer overflow occurred computing total headers length.");

        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING ||
            header->header_value_type == AWS_EVENT_STREAM_HEADER_BYTE_BUF) {
            AWS_FATAL_ASSERT(
                !aws_add_size_checked(headers_len, sizeof(header->header_value_len), &headers_len) &&
                "integer overflow occurred computing total headers length.");
        }

        if (header->header_value_type != AWS_EVENT_STREAM_HEADER_BOOL_FALSE &&
            header->header_value_type != AWS_EVENT_STREAM_HEADER_BOOL_TRUE) {
            AWS_FATAL_ASSERT(
                !aws_add_size_checked(headers_len, header->header_value_len, &headers_len) &&
                "integer overflow occurred computing total headers length.");
        }
    }

    return (uint32_t)headers_len;
}

int aws_event_stream_write_headers_to_buffer_safe(const struct aws_array_list *headers, struct aws_byte_buf *buf) {
    AWS_FATAL_PRECONDITION(buf);

    if (!headers || !aws_array_list_length(headers)) {
        return AWS_OP_SUCCESS;
    }

    size_t headers_count = aws_array_list_length(headers);

    for (size_t i = 0; i < headers_count; ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;

        aws_array_list_get_at_ptr(headers, (void **)&header, i);
        AWS_RETURN_ERROR_IF(
            aws_byte_buf_write_u8(buf, header->header_name_len), AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);
        AWS_RETURN_ERROR_IF(
            aws_byte_buf_write(buf, (uint8_t *)header->header_name, (size_t)header->header_name_len),
            AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);
        AWS_RETURN_ERROR_IF(
            aws_byte_buf_write_u8(buf, (uint8_t)header->header_value_type),
            AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);

        switch (header->header_value_type) {
            case AWS_EVENT_STREAM_HEADER_BOOL_FALSE:
            case AWS_EVENT_STREAM_HEADER_BOOL_TRUE:
                break;
            /* additions of integers here assume the endianness conversion has already happened */
            case AWS_EVENT_STREAM_HEADER_BYTE:
            case AWS_EVENT_STREAM_HEADER_INT16:
            case AWS_EVENT_STREAM_HEADER_INT32:
            case AWS_EVENT_STREAM_HEADER_INT64:
            case AWS_EVENT_STREAM_HEADER_TIMESTAMP:
            case AWS_EVENT_STREAM_HEADER_UUID:
                AWS_RETURN_ERROR_IF(
                    aws_byte_buf_write(buf, header->header_value.static_val, header->header_value_len),
                    AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);
                break;
            case AWS_EVENT_STREAM_HEADER_BYTE_BUF:
            case AWS_EVENT_STREAM_HEADER_STRING:
                AWS_RETURN_ERROR_IF(
                    aws_byte_buf_write_be16(buf, header->header_value_len),
                    AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);
                AWS_RETURN_ERROR_IF(
                    aws_byte_buf_write(buf, header->header_value.variable_len_val, header->header_value_len),
                    AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN);
                break;
            default:
                AWS_FATAL_ASSERT(false && !"Unknown header type!");
                break;
        }
    }

    return AWS_OP_SUCCESS;
}

/* adds the headers represented in the headers list to the buffer.
 returns the new buffer offset for use elsewhere. Assumes buffer length is at least the length of the return value
 from compute_headers_length() */
size_t aws_event_stream_write_headers_to_buffer(const struct aws_array_list *headers, uint8_t *buffer) {
    AWS_FATAL_PRECONDITION(buffer);

    uint32_t min_buffer_len_assumption = aws_event_stream_compute_headers_required_buffer_len(headers);
    struct aws_byte_buf safer_buf = aws_byte_buf_from_empty_array(buffer, min_buffer_len_assumption);

    if (aws_event_stream_write_headers_to_buffer_safe(headers, &safer_buf)) {
        return 0;
    }

    return safer_buf.len;
}

int aws_event_stream_read_headers_from_buffer(
    struct aws_array_list *headers,
    const uint8_t *buffer,
    size_t headers_len) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_FATAL_PRECONDITION(buffer);

    if (AWS_UNLIKELY(headers_len > (size_t)AWS_EVENT_STREAM_MAX_HEADERS_SIZE)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
    }

    struct aws_byte_cursor buffer_cur = aws_byte_cursor_from_array(buffer, headers_len);
    /* iterate the buffer per header. */
    while (buffer_cur.len) {
        struct aws_event_stream_header_value_pair header;
        AWS_ZERO_STRUCT(header);

        /* get the header info from the buffer, make sure to increment buffer offset. */
        aws_byte_cursor_read_u8(&buffer_cur, &header.header_name_len);
        AWS_RETURN_ERROR_IF(header.header_name_len <= INT8_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
        AWS_RETURN_ERROR_IF(
            aws_byte_cursor_read(&buffer_cur, header.header_name, (size_t)header.header_name_len),
            AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
        AWS_RETURN_ERROR_IF(
            aws_byte_cursor_read_u8(&buffer_cur, (uint8_t *)&header.header_value_type),
            AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);

        switch (header.header_value_type) {
            case AWS_EVENT_STREAM_HEADER_BOOL_FALSE:
                header.header_value_len = 0;
                header.header_value.static_val[0] = 0;
                break;
            case AWS_EVENT_STREAM_HEADER_BOOL_TRUE:
                header.header_value_len = 0;
                header.header_value.static_val[0] = 1;
                break;
            case AWS_EVENT_STREAM_HEADER_BYTE:
                header.header_value_len = sizeof(uint8_t);
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read(&buffer_cur, header.header_value.static_val, header.header_value_len),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                break;
            case AWS_EVENT_STREAM_HEADER_INT16:
                header.header_value_len = sizeof(uint16_t);
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read(&buffer_cur, header.header_value.static_val, header.header_value_len),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                break;
            case AWS_EVENT_STREAM_HEADER_INT32:
                header.header_value_len = sizeof(uint32_t);
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read(&buffer_cur, header.header_value.static_val, header.header_value_len),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                break;
            case AWS_EVENT_STREAM_HEADER_INT64:
            case AWS_EVENT_STREAM_HEADER_TIMESTAMP:
                header.header_value_len = sizeof(uint64_t);
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read(&buffer_cur, header.header_value.static_val, header.header_value_len),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                break;
            case AWS_EVENT_STREAM_HEADER_BYTE_BUF:
            case AWS_EVENT_STREAM_HEADER_STRING:
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read_be16(&buffer_cur, &header.header_value_len),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                AWS_RETURN_ERROR_IF(
                    header.header_value_len <= INT16_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
                AWS_RETURN_ERROR_IF(
                    buffer_cur.len >= header.header_value_len, AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                header.header_value.variable_len_val = (uint8_t *)buffer_cur.ptr;
                aws_byte_cursor_advance(&buffer_cur, header.header_value_len);
                break;
            case AWS_EVENT_STREAM_HEADER_UUID:
                header.header_value_len = UUID_LEN;
                AWS_RETURN_ERROR_IF(
                    aws_byte_cursor_read(&buffer_cur, header.header_value.static_val, UUID_LEN),
                    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
                break;
        }

        if (aws_array_list_push_back(headers, (const void *)&header)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

/* initialize message with the arguments
 * the underlying buffer will be allocated and payload will be copied.
 * see specification, this code should simply add these fields according to that.*/
int aws_event_stream_message_init(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    const struct aws_array_list *headers,
    const struct aws_byte_buf *payload) {
    AWS_FATAL_PRECONDITION(message);
    AWS_FATAL_PRECONDITION(alloc);

    size_t payload_len = payload ? payload->len : 0;

    uint32_t headers_length = aws_event_stream_compute_headers_required_buffer_len(headers);

    if (AWS_UNLIKELY(headers_length > AWS_EVENT_STREAM_MAX_HEADERS_SIZE)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
    }

    uint32_t total_length =
        (uint32_t)(AWS_EVENT_STREAM_PRELUDE_LENGTH + headers_length + payload_len + AWS_EVENT_STREAM_TRAILER_LENGTH);

    if (AWS_UNLIKELY(total_length < headers_length || total_length < payload_len)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    if (AWS_UNLIKELY(total_length > AWS_EVENT_STREAM_MAX_MESSAGE_SIZE)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
    }

    message->alloc = alloc;
    aws_byte_buf_init(&message->message_buffer, message->alloc, total_length);

    aws_byte_buf_write_be32(&message->message_buffer, total_length);
    aws_byte_buf_write_be32(&message->message_buffer, headers_length);

    uint32_t running_crc = aws_checksums_crc32(message->message_buffer.buffer, (int)message->message_buffer.len, 0);

    const uint8_t *pre_prelude_marker = message->message_buffer.buffer + message->message_buffer.len;
    size_t pre_prelude_position_marker = message->message_buffer.len;
    aws_byte_buf_write_be32(&message->message_buffer, running_crc);

    if (headers_length) {
        if (aws_event_stream_write_headers_to_buffer_safe(headers, &message->message_buffer)) {
            aws_event_stream_message_clean_up(message);
            return AWS_OP_ERR;
        }
    }

    if (payload) {
        aws_byte_buf_write_from_whole_buffer(&message->message_buffer, *payload);
    }

    running_crc = aws_checksums_crc32(
        pre_prelude_marker, (int)(message->message_buffer.len - pre_prelude_position_marker), running_crc);
    aws_byte_buf_write_be32(&message->message_buffer, running_crc);

    return AWS_OP_SUCCESS;
}

/* add buffer to the message (non-owning). Verify buffer crcs and that length fields are reasonable. */
int aws_event_stream_message_from_buffer(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    struct aws_byte_buf *buffer) {
    AWS_FATAL_PRECONDITION(message);
    AWS_FATAL_PRECONDITION(alloc);
    AWS_FATAL_PRECONDITION(buffer);

    message->alloc = alloc;

    if (AWS_UNLIKELY(buffer->len < AWS_EVENT_STREAM_PRELUDE_LENGTH + AWS_EVENT_STREAM_TRAILER_LENGTH)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
    }

    struct aws_byte_cursor parsing_cur = aws_byte_cursor_from_buf(buffer);

    uint32_t message_length = 0;
    aws_byte_cursor_read_be32(&parsing_cur, &message_length);

    if (AWS_UNLIKELY(message_length != buffer->len)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH);
    }

    if (AWS_UNLIKELY(message_length > AWS_EVENT_STREAM_MAX_MESSAGE_SIZE)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
    }
    /* skip the headers for the moment, we'll handle those later. */
    aws_byte_cursor_advance(&parsing_cur, sizeof(uint32_t));
    uint32_t running_crc = aws_checksums_crc32(buffer->buffer, (int)PRELUDE_CRC_OFFSET, 0);
    uint32_t prelude_crc = 0;
    const uint8_t *start_of_payload_checksum = parsing_cur.ptr;
    size_t start_of_payload_checksum_pos = PRELUDE_CRC_OFFSET;
    aws_byte_cursor_read_be32(&parsing_cur, &prelude_crc);

    if (running_crc != prelude_crc) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE);
    }

    running_crc = aws_checksums_crc32(
        start_of_payload_checksum,
        (int)(message_length - start_of_payload_checksum_pos - AWS_EVENT_STREAM_TRAILER_LENGTH),
        running_crc);
    uint32_t message_crc = aws_read_u32(buffer->buffer + message_length - AWS_EVENT_STREAM_TRAILER_LENGTH);

    if (running_crc != message_crc) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE);
    }

    message->message_buffer = *buffer;
    /* we don't own this buffer, this is a zero allocation/copy path. Setting allocator to null will prevent the
     * clean_up from attempting to free it */
    message->message_buffer.allocator = NULL;

    if (aws_event_stream_message_headers_len(message) >
        message_length - AWS_EVENT_STREAM_PRELUDE_LENGTH - AWS_EVENT_STREAM_TRAILER_LENGTH) {
        AWS_ZERO_STRUCT(message->message_buffer);
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
    }

    return AWS_OP_SUCCESS;
}

/* Verify buffer crcs and that length fields are reasonable. Once that is done, the buffer is copied to the message. */
int aws_event_stream_message_from_buffer_copy(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    const struct aws_byte_buf *buffer) {
    int parse_value = aws_event_stream_message_from_buffer(message, alloc, (struct aws_byte_buf *)buffer);

    if (!parse_value) {
        aws_byte_buf_init_copy(&message->message_buffer, alloc, buffer);
        message->alloc = alloc;
        return AWS_OP_SUCCESS;
    }

    return parse_value;
}

/* if buffer is owned, release the memory. */
void aws_event_stream_message_clean_up(struct aws_event_stream_message *message) {
    aws_byte_buf_clean_up(&message->message_buffer);
}

uint32_t aws_event_stream_message_total_length(const struct aws_event_stream_message *message) {
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(&read_cur, TOTAL_LEN_OFFSET);
    uint32_t total_len = 0;
    aws_byte_cursor_read_be32(&read_cur, &total_len);

    return total_len;
}

uint32_t aws_event_stream_message_headers_len(const struct aws_event_stream_message *message) {
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(&read_cur, HEADER_LEN_OFFSET);

    uint32_t headers_len = 0;
    aws_byte_cursor_read_be32(&read_cur, &headers_len);

    return headers_len;
}

uint32_t aws_event_stream_message_prelude_crc(const struct aws_event_stream_message *message) {
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(&read_cur, PRELUDE_CRC_OFFSET);

    uint32_t prelude_crc = 0;
    aws_byte_cursor_read_be32(&read_cur, &prelude_crc);

    return prelude_crc;
}

int aws_event_stream_message_headers(const struct aws_event_stream_message *message, struct aws_array_list *headers) {
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(&read_cur, AWS_EVENT_STREAM_PRELUDE_LENGTH);

    return aws_event_stream_read_headers_from_buffer(
        headers, read_cur.ptr, aws_event_stream_message_headers_len(message));
}

const uint8_t *aws_event_stream_message_payload(const struct aws_event_stream_message *message) {
    AWS_FATAL_PRECONDITION(message);
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(&read_cur, AWS_EVENT_STREAM_PRELUDE_LENGTH + aws_event_stream_message_headers_len(message));
    return read_cur.ptr;
}

uint32_t aws_event_stream_message_payload_len(const struct aws_event_stream_message *message) {
    AWS_FATAL_PRECONDITION(message);
    return aws_event_stream_message_total_length(message) -
           (AWS_EVENT_STREAM_PRELUDE_LENGTH + aws_event_stream_message_headers_len(message) +
            AWS_EVENT_STREAM_TRAILER_LENGTH);
}

uint32_t aws_event_stream_message_message_crc(const struct aws_event_stream_message *message) {
    AWS_FATAL_PRECONDITION(message);
    struct aws_byte_cursor read_cur = aws_byte_cursor_from_buf(&message->message_buffer);
    aws_byte_cursor_advance(
        &read_cur, aws_event_stream_message_total_length(message) - AWS_EVENT_STREAM_TRAILER_LENGTH);

    uint32_t message_crc = 0;
    aws_byte_cursor_read_be32(&read_cur, &message_crc);

    return message_crc;
}

const uint8_t *aws_event_stream_message_buffer(const struct aws_event_stream_message *message) {
    AWS_FATAL_PRECONDITION(message);
    return message->message_buffer.buffer;
}

#define DEBUG_STR_PRELUDE_TOTAL_LEN "\"total_length\": "
#define DEBUG_STR_PRELUDE_HDRS_LEN "\"headers_length\": "
#define DEBUG_STR_PRELUDE_CRC "\"prelude_crc\": "
#define DEBUG_STR_MESSAGE_CRC "\"message_crc\": "
#define DEBUG_STR_HEADER_NAME "\"name\": "
#define DEBUG_STR_HEADER_VALUE "\"value\": "
#define DEBUG_STR_HEADER_TYPE "\"type\": "

int aws_event_stream_message_to_debug_str(FILE *fd, const struct aws_event_stream_message *message) {
    AWS_FATAL_PRECONDITION(fd);
    AWS_FATAL_PRECONDITION(message);

    struct aws_array_list headers;
    aws_event_stream_headers_list_init(&headers, message->alloc);
    aws_event_stream_message_headers(message, &headers);

    fprintf(
        fd,
        "{\n  " DEBUG_STR_PRELUDE_TOTAL_LEN "%d,\n  " DEBUG_STR_PRELUDE_HDRS_LEN "%d,\n  " DEBUG_STR_PRELUDE_CRC
        "%d,\n",
        aws_event_stream_message_total_length(message),
        aws_event_stream_message_headers_len(message),
        aws_event_stream_message_prelude_crc(message));

    int count = 0;

    uint16_t headers_count = (uint16_t)aws_array_list_length(&headers);

    fprintf(fd, "  \"headers\": [");

    for (uint16_t i = 0; i < headers_count; ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;

        aws_array_list_get_at_ptr(&headers, (void **)&header, i);

        fprintf(fd, "    {\n");

        fprintf(fd, "      " DEBUG_STR_HEADER_NAME "\"");
        fwrite(header->header_name, sizeof(char), (size_t)header->header_name_len, fd);
        fprintf(fd, "\",\n");

        fprintf(fd, "      " DEBUG_STR_HEADER_TYPE "%d,\n", header->header_value_type);

        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_BOOL_FALSE) {
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "false\n");
        } else if (header->header_value_type == AWS_EVENT_STREAM_HEADER_BOOL_TRUE) {
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "true\n");
        } else if (header->header_value_type == AWS_EVENT_STREAM_HEADER_BYTE) {
            int8_t int_value = (int8_t)header->header_value.static_val[0];
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "%d\n", (int)int_value);
        } else if (header->header_value_type == AWS_EVENT_STREAM_HEADER_INT16) {
            int16_t int_value = aws_read_u16(header->header_value.static_val);
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "%d\n", (int)int_value);
        } else if (header->header_value_type == AWS_EVENT_STREAM_HEADER_INT32) {
            int32_t int_value = (int32_t)aws_read_u32(header->header_value.static_val);
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "%d\n", (int)int_value);
        } else if (
            header->header_value_type == AWS_EVENT_STREAM_HEADER_INT64 ||
            header->header_value_type == AWS_EVENT_STREAM_HEADER_TIMESTAMP) {
            int64_t int_value = (int64_t)aws_read_u64(header->header_value.static_val);
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "%lld\n", (long long)int_value);
        } else {
            size_t buffer_len = 0;
            aws_base64_compute_encoded_len(header->header_value_len, &buffer_len);
            char *encoded_buffer = (char *)aws_mem_acquire(message->alloc, buffer_len);

            struct aws_byte_buf encode_output = aws_byte_buf_from_array((uint8_t *)encoded_buffer, buffer_len);

            if (header->header_value_type == AWS_EVENT_STREAM_HEADER_UUID) {
                struct aws_byte_cursor to_encode =
                    aws_byte_cursor_from_array(header->header_value.static_val, header->header_value_len);

                aws_base64_encode(&to_encode, &encode_output);
            } else {
                struct aws_byte_cursor to_encode =
                    aws_byte_cursor_from_array(header->header_value.variable_len_val, header->header_value_len);
                aws_base64_encode(&to_encode, &encode_output);
            }
            fprintf(fd, "      " DEBUG_STR_HEADER_VALUE "\"%s\"\n", encoded_buffer);
            aws_mem_release(message->alloc, encoded_buffer);
        }

        fprintf(fd, "    }");

        if (count < headers_count - 1) {
            fprintf(fd, ",");
        }
        fprintf(fd, "\n");

        count++;
    }
    aws_event_stream_headers_list_cleanup(&headers);
    fprintf(fd, "  ],\n");

    size_t payload_len = aws_event_stream_message_payload_len(message);
    const uint8_t *payload = aws_event_stream_message_payload(message);
    size_t encoded_len = 0;
    aws_base64_compute_encoded_len(payload_len, &encoded_len);
    char *encoded_payload = (char *)aws_mem_acquire(message->alloc, encoded_len);

    struct aws_byte_cursor payload_buffer = aws_byte_cursor_from_array(payload, payload_len);
    struct aws_byte_buf encoded_payload_buffer = aws_byte_buf_from_array((uint8_t *)encoded_payload, encoded_len);

    aws_base64_encode(&payload_buffer, &encoded_payload_buffer);
    fprintf(fd, "  \"payload\": \"%s\",\n", encoded_payload);
    fprintf(fd, "  " DEBUG_STR_MESSAGE_CRC "%d\n}\n", aws_event_stream_message_message_crc(message));

    return AWS_OP_SUCCESS;
}

int aws_event_stream_headers_list_init(struct aws_array_list *headers, struct aws_allocator *allocator) {
    AWS_FATAL_PRECONDITION(headers);
    AWS_FATAL_PRECONDITION(allocator);

    return aws_array_list_init_dynamic(headers, allocator, 4, sizeof(struct aws_event_stream_header_value_pair));
}

void aws_event_stream_headers_list_cleanup(struct aws_array_list *headers) {
    AWS_FATAL_PRECONDITION(headers);

    if (AWS_UNLIKELY(!headers || !aws_array_list_is_valid(headers))) {
        return;
    }

    for (size_t i = 0; i < aws_array_list_length(headers); ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;
        aws_array_list_get_at_ptr(headers, (void **)&header, i);

        if (header->value_owned) {
            aws_mem_release(headers->alloc, (void *)header->header_value.variable_len_val);
        }
    }

    aws_array_list_clean_up(headers);
}

static int s_add_variable_len_header(
    struct aws_array_list *headers,
    struct aws_event_stream_header_value_pair *header,
    const char *name,
    uint8_t name_len,
    uint8_t *value,
    uint16_t value_len,
    int8_t copy) {

    memcpy((void *)header->header_name, (void *)name, (size_t)name_len);

    if (value_len != 0 && copy) {
        header->header_value.variable_len_val = aws_mem_acquire(headers->alloc, value_len);
        header->value_owned = 1;
        memcpy((void *)header->header_value.variable_len_val, (void *)value, value_len);
    } else {
        header->value_owned = 0;
        header->header_value.variable_len_val = value;
    }

    if (aws_array_list_push_back(headers, (void *)header)) {
        if (header->value_owned) {
            aws_mem_release(headers->alloc, (void *)header->header_value.variable_len_val);
        }
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_event_stream_add_string_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    const char *value,
    uint16_t value_len,
    int8_t copy) {
    AWS_FATAL_PRECONDITION(headers);
    AWS_RETURN_ERROR_IF(
        name_len <= AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
    AWS_RETURN_ERROR_IF(value_len <= INT16_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
    struct aws_event_stream_header_value_pair header = {
        .header_name_len = name_len,
        .header_value_len = value_len,
        .value_owned = copy,
        .header_value_type = AWS_EVENT_STREAM_HEADER_STRING,
    };

    return s_add_variable_len_header(headers, &header, name, name_len, (uint8_t *)value, value_len, copy);
}

struct aws_event_stream_header_value_pair aws_event_stream_create_string_header(
    struct aws_byte_cursor name,
    struct aws_byte_cursor value) {
    AWS_FATAL_PRECONDITION(name.len <= AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX);
    AWS_FATAL_PRECONDITION(value.len <= INT16_MAX);

    struct aws_event_stream_header_value_pair header = {
        .header_value_type = AWS_EVENT_STREAM_HEADER_STRING,
        .header_value.variable_len_val = value.ptr,
        .header_value_len = (uint16_t)value.len,
        .header_name_len = (uint8_t)name.len,
        .value_owned = 0,
    };

    memcpy(header.header_name, name.ptr, name.len);

    return header;
}

struct aws_event_stream_header_value_pair aws_event_stream_create_int32_header(
    struct aws_byte_cursor name,
    int32_t value) {
    AWS_FATAL_PRECONDITION(name.len <= AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX);

    struct aws_event_stream_header_value_pair header = {
        .header_value_type = AWS_EVENT_STREAM_HEADER_INT32,
        .header_value_len = (uint16_t)sizeof(int32_t),
        .header_name_len = (uint8_t)name.len,
        .value_owned = 0,
    };

    memcpy(header.header_name, name.ptr, name.len);
    aws_write_u32((uint32_t)value, header.header_value.static_val);

    return header;
}

int aws_event_stream_add_bool_header(struct aws_array_list *headers, const char *name, uint8_t name_len, int8_t value) {
    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_bool_header_by_cursor(headers, name_cursor, value != 0);
}

#define AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name_cursor)                                                      \
    AWS_FATAL_PRECONDITION(name_cursor.len > 0);                                                                       \
    AWS_FATAL_PRECONDITION(name_cursor.ptr != NULL);                                                                   \
    AWS_RETURN_ERROR_IF(                                                                                               \
        name_cursor.len <= AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);

int aws_event_stream_add_bool_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    bool value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = 0,
        .value_owned = 0,
        .header_value_type = value ? AWS_EVENT_STREAM_HEADER_BOOL_TRUE : AWS_EVENT_STREAM_HEADER_BOOL_FALSE,
    };

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_byte_header(struct aws_array_list *headers, const char *name, uint8_t name_len, int8_t value) {
    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_byte_header_by_cursor(headers, name_cursor, value);
}

int aws_event_stream_add_byte_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int8_t value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = 1,
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_BYTE,
    };
    header.header_value.static_val[0] = (uint8_t)value;

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_int16_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int16_t value) {

    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_int16_header_by_cursor(headers, name_cursor, value);
}

int aws_event_stream_add_int16_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int16_t value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = sizeof(value),
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_INT16,
    };

    aws_write_u16((uint16_t)value, header.header_value.static_val);

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_int32_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int32_t value) {

    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_int32_header_by_cursor(headers, name_cursor, value);
}

int aws_event_stream_add_int32_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int32_t value) {
    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = sizeof(value),
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_INT32,
    };

    aws_write_u32((uint32_t)value, header.header_value.static_val);

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_int64_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int64_t value) {

    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_int64_header_by_cursor(headers, name_cursor, value);
}

int aws_event_stream_add_int64_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int64_t value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = sizeof(value),
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_INT64,
    };

    aws_write_u64((uint64_t)value, header.header_value.static_val);

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_string_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);
    AWS_RETURN_ERROR_IF(value.len <= INT16_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = (uint16_t)value.len,
        .value_owned = 1,
        .header_value_type = AWS_EVENT_STREAM_HEADER_STRING,
    };

    return s_add_variable_len_header(
        headers, &header, (const char *)name.ptr, (uint8_t)name.len, value.ptr, (uint16_t)value.len, 1);
}

int aws_event_stream_add_byte_buf_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);
    AWS_RETURN_ERROR_IF(value.len <= INT16_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = (uint16_t)value.len,
        .value_owned = 1,
        .header_value_type = AWS_EVENT_STREAM_HEADER_BYTE_BUF,
    };

    return s_add_variable_len_header(
        headers, &header, (const char *)name.ptr, (uint8_t)name.len, value.ptr, (uint16_t)value.len, 1);
}

int aws_event_stream_add_timestamp_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int64_t value) {

    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);

    return aws_event_stream_add_timestamp_header_by_cursor(headers, name_cursor, value);
}

int aws_event_stream_add_timestamp_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int64_t value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = sizeof(value),
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_TIMESTAMP,
    };

    aws_write_u64((uint64_t)value, header.header_value.static_val);

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_uuid_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    const uint8_t *value) {

    struct aws_byte_cursor name_cursor = aws_byte_cursor_from_array(name, (size_t)name_len);
    struct aws_byte_cursor value_cursor = aws_byte_cursor_from_array(value, (size_t)UUID_LEN);

    return aws_event_stream_add_uuid_header_by_cursor(headers, name_cursor, value_cursor);
}

int aws_event_stream_add_uuid_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value) {

    AWS_FATAL_PRECONDITION(headers);
    AWS_EVENT_STREAM_VALIDATE_HEADER_NAME_CURSOR(name);
    AWS_RETURN_ERROR_IF(value.len == UUID_LEN, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = (uint8_t)name.len,
        .header_value_len = UUID_LEN,
        .value_owned = 0,
        .header_value_type = AWS_EVENT_STREAM_HEADER_UUID,
    };

    memcpy((void *)header.header_name, (void *)name.ptr, (size_t)name.len);
    memcpy((void *)header.header_value.static_val, value.ptr, UUID_LEN);

    return aws_array_list_push_back(headers, (void *)&header);
}

int aws_event_stream_add_bytebuf_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    uint8_t *value,
    uint16_t value_len,
    int8_t copy) {
    AWS_FATAL_PRECONDITION(headers);
    AWS_FATAL_PRECONDITION(name);
    AWS_RETURN_ERROR_IF(
        name_len <= AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);
    AWS_RETURN_ERROR_IF(value_len <= INT16_MAX, AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN);

    struct aws_event_stream_header_value_pair header = {
        .header_name_len = name_len,
        .header_value_len = value_len,
        .value_owned = copy,
        .header_value_type = AWS_EVENT_STREAM_HEADER_BYTE_BUF,
    };

    return s_add_variable_len_header(headers, &header, name, name_len, value, value_len, copy);
}

int aws_event_stream_add_header(
    struct aws_array_list *headers,
    const struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(headers);
    AWS_FATAL_PRECONDITION(header);

    struct aws_event_stream_header_value_pair header_copy = *header;

    if (header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING ||
        header->header_value_type == AWS_EVENT_STREAM_HEADER_BYTE_BUF) {
        return s_add_variable_len_header(
            headers,
            &header_copy,
            header->header_name,
            header->header_name_len,
            header->header_value.variable_len_val,
            header->header_value_len,
            1); /* Copy the header value */
    }

    return aws_array_list_push_back(headers, (void *)&header_copy);
}

struct aws_byte_buf aws_event_stream_header_name(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return aws_byte_buf_from_array((uint8_t *)header->header_name, header->header_name_len);
}

int8_t aws_event_stream_header_value_as_byte(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return (int8_t)header->header_value.static_val[0];
}

struct aws_byte_buf aws_event_stream_header_value_as_string(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return aws_event_stream_header_value_as_bytebuf(header);
}

int8_t aws_event_stream_header_value_as_bool(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return header->header_value_type == AWS_EVENT_STREAM_HEADER_BOOL_TRUE ? (int8_t)1 : (int8_t)0;
}

int16_t aws_event_stream_header_value_as_int16(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return (int16_t)aws_read_u16(header->header_value.static_val);
}

int32_t aws_event_stream_header_value_as_int32(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return (int32_t)aws_read_u32(header->header_value.static_val);
}

int64_t aws_event_stream_header_value_as_int64(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);
    return (int64_t)aws_read_u64(header->header_value.static_val);
}

struct aws_byte_buf aws_event_stream_header_value_as_bytebuf(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);
    return aws_byte_buf_from_array(header->header_value.variable_len_val, header->header_value_len);
}

int64_t aws_event_stream_header_value_as_timestamp(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);
    return aws_event_stream_header_value_as_int64(header);
}

struct aws_byte_buf aws_event_stream_header_value_as_uuid(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);
    return aws_byte_buf_from_array(header->header_value.static_val, UUID_LEN);
}

uint16_t aws_event_stream_header_value_length(struct aws_event_stream_header_value_pair *header) {
    AWS_FATAL_PRECONDITION(header);

    return header->header_value_len;
}

static struct aws_event_stream_message_prelude s_empty_prelude = {.total_len = 0, .headers_len = 0, .prelude_crc = 0};

static void s_reset_header_state(struct aws_event_stream_streaming_decoder *decoder, uint8_t free_header_data) {

    if (free_header_data && decoder->current_header.value_owned) {
        aws_mem_release(decoder->alloc, (void *)decoder->current_header.header_value.variable_len_val);
    }

    memset((void *)&decoder->current_header, 0, sizeof(struct aws_event_stream_header_value_pair));
}

static void s_reset_state(struct aws_event_stream_streaming_decoder *decoder);

static int s_headers_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed);

static int s_read_header_value(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {

    size_t current_pos = decoder->message_pos;

    /* amount that we've already read */
    size_t length_read = current_pos - decoder->current_header_value_offset;
    struct aws_event_stream_header_value_pair *current_header = &decoder->current_header;

    if (!length_read && (current_header->header_value_type == AWS_EVENT_STREAM_HEADER_BYTE_BUF ||
                         current_header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING)) {
        /* save an allocation, this can only happen if the data we were handed is larger than the length of the header
         * value. we don't really need to handle offsets in this case. This expects the user is living by the contract
         * that they cannot act like they own this memory beyond the lifetime of their callback, and they should not
         * mutate it */
        if (len >= current_header->header_value_len) {
            /* this part works regardless of type since the layout of the union will line up. */
            current_header->header_value.variable_len_val = (uint8_t *)data;
            current_header->value_owned = 0;
            decoder->on_header(decoder, &decoder->prelude, &decoder->current_header, decoder->user_context);
            *processed += current_header->header_value_len;
            decoder->message_pos += current_header->header_value_len;
            decoder->running_crc =
                aws_checksums_crc32(data, (int)current_header->header_value_len, decoder->running_crc);

            s_reset_header_state(decoder, 1);
            decoder->state = s_headers_state;
            return AWS_OP_SUCCESS;
        }

        /* a possible optimization later would be to only allocate this once, and then keep reusing the same buffer. for
         * subsequent messages.*/
        current_header->header_value.variable_len_val =
            aws_mem_acquire(decoder->alloc, decoder->current_header.header_value_len);

        current_header->value_owned = 1;
    }

    size_t max_read =
        len >= current_header->header_value_len - length_read ? current_header->header_value_len - length_read : len;

    const uint8_t *header_value_alias = current_header->header_value_type == AWS_EVENT_STREAM_HEADER_BYTE_BUF ||
                                                current_header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING
                                            ? current_header->header_value.variable_len_val
                                            : current_header->header_value.static_val;

    memcpy((void *)(header_value_alias + length_read), data, max_read);
    decoder->running_crc = aws_checksums_crc32(data, (int)max_read, decoder->running_crc);

    *processed += max_read;
    decoder->message_pos += max_read;
    length_read += max_read;

    if (length_read == current_header->header_value_len) {
        decoder->on_header(decoder, &decoder->prelude, current_header, decoder->user_context);
        s_reset_header_state(decoder, 1);
        decoder->state = s_headers_state;
    }

    return AWS_OP_SUCCESS;
}

static int s_read_header_value_len(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {
    size_t current_pos = decoder->message_pos;

    size_t length_portion_read = current_pos - decoder->current_header_value_offset;

    if (length_portion_read < sizeof(uint16_t)) {
        size_t max_to_read =
            len > sizeof(uint16_t) - length_portion_read ? sizeof(uint16_t) - length_portion_read : len;
        memcpy(decoder->working_buffer + length_portion_read, data, max_to_read);
        decoder->running_crc = aws_checksums_crc32(data, (int)max_to_read, decoder->running_crc);

        *processed += max_to_read;
        decoder->message_pos += max_to_read;

        length_portion_read = decoder->message_pos - decoder->current_header_value_offset;
    }

    if (length_portion_read == sizeof(uint16_t)) {
        decoder->current_header.header_value_len = aws_read_u16(decoder->working_buffer);
        decoder->current_header_value_offset = decoder->message_pos;
        decoder->state = s_read_header_value;
    }

    return AWS_OP_SUCCESS;
}

static int s_read_header_type(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {
    (void)len;
    uint8_t type = *data;
    decoder->running_crc = aws_checksums_crc32(data, 1, decoder->running_crc);
    *processed += 1;
    decoder->message_pos++;
    decoder->current_header_value_offset++;
    struct aws_event_stream_header_value_pair *current_header = &decoder->current_header;

    current_header->header_value_type = (enum aws_event_stream_header_value_type)type;

    switch (type) {
        case AWS_EVENT_STREAM_HEADER_STRING:
        case AWS_EVENT_STREAM_HEADER_BYTE_BUF:
            decoder->state = s_read_header_value_len;
            break;
        case AWS_EVENT_STREAM_HEADER_BOOL_FALSE:
            current_header->header_value_len = 0;
            current_header->header_value.static_val[0] = 0;
            decoder->on_header(decoder, &decoder->prelude, current_header, decoder->user_context);
            s_reset_header_state(decoder, 1);
            decoder->state = s_headers_state;
            break;
        case AWS_EVENT_STREAM_HEADER_BOOL_TRUE:
            current_header->header_value_len = 0;
            current_header->header_value.static_val[0] = 1;
            decoder->on_header(decoder, &decoder->prelude, current_header, decoder->user_context);
            s_reset_header_state(decoder, 1);
            decoder->state = s_headers_state;
            break;
        case AWS_EVENT_STREAM_HEADER_BYTE:
            current_header->header_value_len = 1;
            decoder->state = s_read_header_value;
            break;
        case AWS_EVENT_STREAM_HEADER_INT16:
            current_header->header_value_len = sizeof(uint16_t);
            decoder->state = s_read_header_value;
            break;
        case AWS_EVENT_STREAM_HEADER_INT32:
            current_header->header_value_len = sizeof(uint32_t);
            decoder->state = s_read_header_value;
            break;
        case AWS_EVENT_STREAM_HEADER_INT64:
        case AWS_EVENT_STREAM_HEADER_TIMESTAMP:
            current_header->header_value_len = sizeof(uint64_t);
            decoder->state = s_read_header_value;
            break;
        case AWS_EVENT_STREAM_HEADER_UUID:
            current_header->header_value_len = 16;
            decoder->state = s_read_header_value;
            break;
        default:
            return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_UNKNOWN_HEADER_TYPE);
    }

    return AWS_OP_SUCCESS;
}

static int s_read_header_name(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {
    size_t current_pos = decoder->message_pos;

    size_t length_read = current_pos - decoder->current_header_name_offset;

    size_t max_read = len >= decoder->current_header.header_name_len - length_read
                          ? decoder->current_header.header_name_len - length_read
                          : len;
    memcpy((void *)(decoder->current_header.header_name + length_read), data, max_read);
    decoder->running_crc = aws_checksums_crc32(data, (int)max_read, decoder->running_crc);

    *processed += max_read;
    decoder->message_pos += max_read;
    length_read += max_read;

    if (length_read == decoder->current_header.header_name_len) {
        decoder->state = s_read_header_type;
        decoder->current_header_value_offset = decoder->message_pos;
    }

    return AWS_OP_SUCCESS;
}

static int s_read_header_name_len(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {
    (void)len;
    decoder->current_header.header_name_len = *data;
    decoder->message_pos++;
    decoder->current_header_name_offset++;
    *processed += 1;
    decoder->state = s_read_header_name;
    decoder->running_crc = aws_checksums_crc32(data, 1, decoder->running_crc);

    return AWS_OP_SUCCESS;
}

static int s_start_header(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) /* NOLINT */ {
    (void)data;
    (void)len;
    (void)processed;
    decoder->state = s_read_header_name_len;
    decoder->current_header_name_offset = decoder->message_pos;

    return AWS_OP_SUCCESS;
}

static int s_payload_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed);

/*Handles the initial state for header parsing.
  will oscillate between multiple other states as well.
  after all headers have been handled, payload will be set as the next state. */
static int s_headers_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) /* NOLINT */ {
    (void)data;
    (void)len;
    (void)processed;

    size_t current_pos = decoder->message_pos;

    size_t headers_boundary = decoder->prelude.headers_len + AWS_EVENT_STREAM_PRELUDE_LENGTH;

    if (current_pos < headers_boundary) {
        decoder->state = s_start_header;
        return AWS_OP_SUCCESS;
    }

    if (current_pos == headers_boundary) {
        decoder->state = s_payload_state;
        return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_PARSER_ILLEGAL_STATE);
}

/* handles reading the trailer. Once it has been read, it will be compared to the running checksum. If successful,
 * the state will be reset. */
static int s_read_trailer_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {

    size_t remaining_amount = decoder->prelude.total_len - decoder->message_pos;
    size_t segment_length = len > remaining_amount ? remaining_amount : len;
    size_t offset = sizeof(uint32_t) - remaining_amount;
    memcpy(decoder->working_buffer + offset, data, segment_length);
    decoder->message_pos += segment_length;
    *processed += segment_length;

    if (decoder->message_pos == decoder->prelude.total_len) {
        uint32_t message_crc = aws_read_u32(decoder->working_buffer);

        if (message_crc == decoder->running_crc) {
            if (decoder->on_complete) {
                decoder->on_complete(decoder, message_crc, decoder->user_context);
            }
            s_reset_state(decoder);
        } else {
            char error_message[70];
            snprintf(
                error_message,
                sizeof(error_message),
                "CRC Mismatch. message_crc was 0x08%" PRIX32 ", but computed 0x08%" PRIX32,
                message_crc,
                decoder->running_crc);
            aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE);
            decoder->on_error(
                decoder,
                &decoder->prelude,
                AWS_ERROR_EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE,
                error_message,
                decoder->user_context);
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

/* handles the reading of the payload up to the final checksum. Sets read_trailer_state as the new state once
 * the payload has been processed. */
static int s_payload_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {

    if (decoder->message_pos < decoder->prelude.total_len - AWS_EVENT_STREAM_TRAILER_LENGTH) {
        size_t remaining_amount = decoder->prelude.total_len - decoder->message_pos - AWS_EVENT_STREAM_TRAILER_LENGTH;
        size_t segment_length = len > remaining_amount ? remaining_amount : len;
        int8_t final_segment =
            (segment_length + decoder->message_pos) == (decoder->prelude.total_len - AWS_EVENT_STREAM_TRAILER_LENGTH);
        struct aws_byte_buf payload_buf = aws_byte_buf_from_array(data, segment_length);
        decoder->on_payload(decoder, &payload_buf, final_segment, decoder->user_context);
        decoder->message_pos += segment_length;
        decoder->running_crc = aws_checksums_crc32(data, (int)segment_length, decoder->running_crc);
        *processed += segment_length;
    }

    if (decoder->message_pos == decoder->prelude.total_len - AWS_EVENT_STREAM_TRAILER_LENGTH) {
        decoder->state = s_read_trailer_state;
    }

    return AWS_OP_SUCCESS;
}

/* Parses the payload and verifies checksums. Sets the next state if successful. */
static int s_verify_prelude_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) /* NOLINT */ {
    (void)data;
    (void)len;
    (void)processed;

    decoder->prelude.headers_len = aws_read_u32(decoder->working_buffer + HEADER_LEN_OFFSET);
    decoder->prelude.prelude_crc = aws_read_u32(decoder->working_buffer + PRELUDE_CRC_OFFSET);
    decoder->prelude.total_len = aws_read_u32(decoder->working_buffer + TOTAL_LEN_OFFSET);

    decoder->running_crc = aws_checksums_crc32(decoder->working_buffer, PRELUDE_CRC_OFFSET, 0);

    if (AWS_LIKELY(decoder->running_crc == decoder->prelude.prelude_crc)) {

        if (AWS_UNLIKELY(
                decoder->prelude.headers_len > AWS_EVENT_STREAM_MAX_HEADERS_SIZE ||
                decoder->prelude.total_len > AWS_EVENT_STREAM_MAX_MESSAGE_SIZE)) {
            aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
            char error_message[] = "Maximum message field size exceeded";

            decoder->on_error(
                decoder,
                &decoder->prelude,
                AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED,
                error_message,
                decoder->user_context);
            return AWS_OP_ERR;
        }

        /* Should only call on_prelude() after passing crc check and limitation check, otherwise call on_prelude() with
         * incorrect prelude is error prune. */
        decoder->on_prelude(decoder, &decoder->prelude, decoder->user_context);

        decoder->running_crc = aws_checksums_crc32(
            decoder->working_buffer + PRELUDE_CRC_OFFSET,
            (int)sizeof(decoder->prelude.prelude_crc),
            decoder->running_crc);
        memset(decoder->working_buffer, 0, sizeof(decoder->working_buffer));
        decoder->state = decoder->prelude.headers_len > 0 ? s_headers_state : s_payload_state;
    } else {
        char error_message[70];
        snprintf(
            error_message,
            sizeof(error_message),
            "CRC Mismatch. prelude_crc was 0x08%" PRIX32 ", but computed 0x08%" PRIX32,
            decoder->prelude.prelude_crc,
            decoder->running_crc);

        aws_raise_error(AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE);
        decoder->on_error(
            decoder,
            &decoder->prelude,
            AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE,
            error_message,
            decoder->user_context);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/* initial state handles up to the reading of the prelude */
static int s_start_state(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed) {

    size_t previous_position = decoder->message_pos;
    if (decoder->message_pos < AWS_EVENT_STREAM_PRELUDE_LENGTH) {
        if (len >= AWS_EVENT_STREAM_PRELUDE_LENGTH - decoder->message_pos) {
            memcpy(
                decoder->working_buffer + decoder->message_pos,
                data,
                AWS_EVENT_STREAM_PRELUDE_LENGTH - decoder->message_pos);
            decoder->message_pos += AWS_EVENT_STREAM_PRELUDE_LENGTH - decoder->message_pos;
        } else {
            memcpy(decoder->working_buffer + decoder->message_pos, data, len);
            decoder->message_pos += len;
        }

        *processed += decoder->message_pos - previous_position;
    }

    if (decoder->message_pos == AWS_EVENT_STREAM_PRELUDE_LENGTH) {
        decoder->state = s_verify_prelude_state;
    }

    return AWS_OP_SUCCESS;
}

static void s_reset_state(struct aws_event_stream_streaming_decoder *decoder) {
    memset(decoder->working_buffer, 0, sizeof(decoder->working_buffer));
    decoder->message_pos = 0;
    decoder->running_crc = 0;
    decoder->current_header_name_offset = 0;
    decoder->current_header_value_offset = 0;
    AWS_ZERO_STRUCT(decoder->current_header);
    decoder->prelude = s_empty_prelude;
    decoder->state = s_start_state;
}

void aws_event_stream_streaming_decoder_init_from_options(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_allocator *allocator,
    const struct aws_event_stream_streaming_decoder_options *options) {
    AWS_ASSERT(decoder);
    AWS_ASSERT(allocator);
    AWS_ASSERT(options);
    AWS_ASSERT(options->on_error);
    AWS_ASSERT(options->on_header);
    AWS_ASSERT(options->on_payload_segment);
    AWS_ASSERT(options->on_prelude);
    AWS_ASSERT(options->on_prelude);

    s_reset_state(decoder);
    decoder->alloc = allocator;
    decoder->on_error = options->on_error;
    decoder->on_header = options->on_header;
    decoder->on_payload = options->on_payload_segment;
    decoder->on_prelude = options->on_prelude;
    decoder->on_complete = options->on_complete;
    decoder->user_context = options->user_data;
}

void aws_event_stream_streaming_decoder_init(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_allocator *alloc,
    aws_event_stream_process_on_payload_segment_fn *on_payload_segment,
    aws_event_stream_prelude_received_fn *on_prelude,
    aws_event_stream_header_received_fn *on_header,
    aws_event_stream_on_error_fn *on_error,
    void *user_data) {

    struct aws_event_stream_streaming_decoder_options decoder_options = {
        .on_payload_segment = on_payload_segment,
        .on_prelude = on_prelude,
        .on_header = on_header,
        .on_error = on_error,
        .user_data = user_data,
    };
    aws_event_stream_streaming_decoder_init_from_options(decoder, alloc, &decoder_options);
}

void aws_event_stream_streaming_decoder_clean_up(struct aws_event_stream_streaming_decoder *decoder) {
    s_reset_state(decoder);
    decoder->on_error = 0;
    decoder->on_header = 0;
    decoder->on_payload = 0;
    decoder->on_prelude = 0;
    decoder->user_context = 0;
    decoder->on_complete = 0;
}

/* Simply sends the data to the state machine until all has been processed or an error is returned. */
int aws_event_stream_streaming_decoder_pump(
    struct aws_event_stream_streaming_decoder *decoder,
    const struct aws_byte_buf *data) {

    size_t processed = 0;
    int err_val = 0;
    while (!err_val && data->buffer && data->len && processed < data->len) {
        err_val = decoder->state(decoder, data->buffer + processed, data->len - processed, &processed);
    }

    return err_val;
}
#ifdef _MSC_VER
#    pragma warning(pop)
#endif
