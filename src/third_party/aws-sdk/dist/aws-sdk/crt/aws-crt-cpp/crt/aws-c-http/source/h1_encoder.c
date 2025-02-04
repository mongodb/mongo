/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/h1_encoder.h>
#include <aws/http/private/strutil.h>
#include <aws/http/status_code.h>
#include <aws/io/logging.h>
#include <aws/io/stream.h>

#include <inttypes.h>

#define ENCODER_LOGF(level, encoder, text, ...)                                                                        \
    AWS_LOGF_##level(AWS_LS_HTTP_STREAM, "id=%p: " text, (void *)encoder->current_stream, __VA_ARGS__)
#define ENCODER_LOG(level, encoder, text) ENCODER_LOGF(level, encoder, "%s", text)

#define MAX_ASCII_HEX_CHUNK_STR_SIZE (sizeof(uint64_t) * 2 + 1)
#define CRLF_SIZE 2

/**
 * Scan headers to detect errors and determine anything we'll need to know later (ex: total length).
 */
static int s_scan_outgoing_headers(
    struct aws_h1_encoder_message *encoder_message,
    const struct aws_http_message *message,
    size_t *out_header_lines_len,
    bool body_headers_ignored,
    bool body_headers_forbidden) {

    size_t total = 0;
    bool has_body_stream = aws_http_message_get_body_stream(message);
    bool has_content_length_header = false;
    bool has_transfer_encoding_header = false;

    const size_t num_headers = aws_http_message_get_header_count(message);
    for (size_t i = 0; i < num_headers; ++i) {
        struct aws_http_header header;
        aws_http_message_get_header(message, &header, i);

        /* Validate header field-name (RFC-7230 3.2): field-name = token */
        if (!aws_strutil_is_http_token(header.name)) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Header name is invalid");
            return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_NAME);
        }

        /* Validate header field-value.
         * The value itself isn't supposed to have whitespace on either side,
         * but we'll trim it off before validation so we don't start needlessly
         * failing requests that used to work before we added validation.
         * This should be OK because field-value can be sent with any amount
         * of whitespace around it, which the other side will just ignore (RFC-7230 3.2):
         * header-field = field-name ":" OWS field-value OWS */
        struct aws_byte_cursor field_value = aws_strutil_trim_http_whitespace(header.value);
        if (!aws_strutil_is_http_field_value(field_value)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=static: Header '" PRInSTR "' has invalid value",
                AWS_BYTE_CURSOR_PRI(header.name));
            return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
        }

        enum aws_http_header_name name_enum = aws_http_str_to_header_name(header.name);
        switch (name_enum) {
            case AWS_HTTP_HEADER_CONNECTION: {
                if (aws_byte_cursor_eq_c_str(&field_value, "close")) {
                    encoder_message->has_connection_close_header = true;
                }
            } break;
            case AWS_HTTP_HEADER_CONTENT_LENGTH: {
                has_content_length_header = true;
                if (aws_byte_cursor_utf8_parse_u64(field_value, &encoder_message->content_length)) {
                    AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Invalid Content-Length");
                    return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
                }
            } break;
            case AWS_HTTP_HEADER_TRANSFER_ENCODING: {
                has_transfer_encoding_header = true;
                if (0 == field_value.len) {
                    AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Transfer-Encoding must include a valid value");
                    return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
                }
                struct aws_byte_cursor substr;
                AWS_ZERO_STRUCT(substr);
                while (aws_byte_cursor_next_split(&field_value, ',', &substr)) {
                    struct aws_byte_cursor trimmed = aws_strutil_trim_http_whitespace(substr);
                    if (0 == trimmed.len) {
                        AWS_LOGF_ERROR(
                            AWS_LS_HTTP_STREAM,
                            "id=static: Transfer-Encoding header whitespace only "
                            "comma delimited header value");
                        return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
                    }
                    if (encoder_message->has_chunked_encoding_header) {
                        AWS_LOGF_ERROR(
                            AWS_LS_HTTP_STREAM, "id=static: Transfer-Encoding header must end with \"chunked\"");
                        return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
                    }
                    if (aws_byte_cursor_eq_c_str(&trimmed, "chunked")) {
                        encoder_message->has_chunked_encoding_header = true;
                    }
                }
            } break;
            default:
                break;
        }

        /* header-line: "{name}: {value}\r\n" */
        int err = 0;
        err |= aws_add_size_checked(header.name.len, total, &total);
        err |= aws_add_size_checked(header.value.len, total, &total);
        err |= aws_add_size_checked(4, total, &total); /* ": " + "\r\n" */
        if (err) {
            return AWS_OP_ERR;
        }
    }

    if (!encoder_message->has_chunked_encoding_header && has_transfer_encoding_header) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Transfer-Encoding header must include \"chunked\"");
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
    }

    /* Per RFC 7230: A sender MUST NOT send a Content-Length header field in any message that contains a
     * Transfer-Encoding header field. */
    if (encoder_message->has_chunked_encoding_header && has_content_length_header) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=static: Both Content-Length and Transfer-Encoding are set. Only one may be used");
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
    }

    if (encoder_message->has_chunked_encoding_header && has_body_stream) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=static: Both Transfer-Encoding chunked header and body stream is set. "
            "chunked data must use the chunk API to write the body stream.");
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_BODY_STREAM);
    }

    if (body_headers_forbidden && (encoder_message->content_length > 0 || has_transfer_encoding_header)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=static: Transfer-Encoding or Content-Length headers may not be present in such a message");
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_FIELD);
    }

    if (body_headers_ignored) {
        /* Don't send body, no matter what the headers are */
        encoder_message->content_length = 0;
        encoder_message->has_chunked_encoding_header = false;
    }

    if (encoder_message->content_length > 0 && !has_body_stream) {
        return aws_raise_error(AWS_ERROR_HTTP_MISSING_BODY_STREAM);
    }

    *out_header_lines_len = total;
    return AWS_OP_SUCCESS;
}

static int s_scan_outgoing_trailer(const struct aws_http_headers *headers, size_t *out_size) {
    const size_t num_headers = aws_http_headers_count(headers);
    size_t total = 0;
    for (size_t i = 0; i < num_headers; i++) {
        struct aws_http_header header;
        aws_http_headers_get_index(headers, i, &header);
        /* Validate header field-name (RFC-7230 3.2): field-name = token */
        if (!aws_strutil_is_http_token(header.name)) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Header name is invalid");
            return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_NAME);
        }

        /* Validate header field-value.
         * The value itself isn't supposed to have whitespace on either side,
         * but we'll trim it off before validation so we don't start needlessly
         * failing requests that used to work before we added validation.
         * This should be OK because field-value can be sent with any amount
         * of whitespace around it, which the other side will just ignore (RFC-7230 3.2):
         * header-field = field-name ":" OWS field-value OWS */
        struct aws_byte_cursor field_value = aws_strutil_trim_http_whitespace(header.value);
        if (!aws_strutil_is_http_field_value(field_value)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=static: Header '" PRInSTR "' has invalid value",
                AWS_BYTE_CURSOR_PRI(header.name));
            return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_VALUE);
        }

        enum aws_http_header_name name_enum = aws_http_str_to_header_name(header.name);
        if (name_enum == AWS_HTTP_HEADER_TRANSFER_ENCODING || name_enum == AWS_HTTP_HEADER_CONTENT_LENGTH ||
            name_enum == AWS_HTTP_HEADER_HOST || name_enum == AWS_HTTP_HEADER_EXPECT ||
            name_enum == AWS_HTTP_HEADER_CACHE_CONTROL || name_enum == AWS_HTTP_HEADER_MAX_FORWARDS ||
            name_enum == AWS_HTTP_HEADER_PRAGMA || name_enum == AWS_HTTP_HEADER_RANGE ||
            name_enum == AWS_HTTP_HEADER_TE || name_enum == AWS_HTTP_HEADER_CONTENT_ENCODING ||
            name_enum == AWS_HTTP_HEADER_CONTENT_TYPE || name_enum == AWS_HTTP_HEADER_CONTENT_RANGE ||
            name_enum == AWS_HTTP_HEADER_TRAILER || name_enum == AWS_HTTP_HEADER_WWW_AUTHENTICATE ||
            name_enum == AWS_HTTP_HEADER_AUTHORIZATION || name_enum == AWS_HTTP_HEADER_PROXY_AUTHENTICATE ||
            name_enum == AWS_HTTP_HEADER_PROXY_AUTHORIZATION || name_enum == AWS_HTTP_HEADER_SET_COOKIE ||
            name_enum == AWS_HTTP_HEADER_COOKIE || name_enum == AWS_HTTP_HEADER_AGE ||
            name_enum == AWS_HTTP_HEADER_EXPIRES || name_enum == AWS_HTTP_HEADER_DATE ||
            name_enum == AWS_HTTP_HEADER_LOCATION || name_enum == AWS_HTTP_HEADER_RETRY_AFTER ||
            name_enum == AWS_HTTP_HEADER_VARY || name_enum == AWS_HTTP_HEADER_WARNING) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=static: Trailing Header '" PRInSTR "' has invalid value",
                AWS_BYTE_CURSOR_PRI(header.name));
            return aws_raise_error(AWS_ERROR_HTTP_INVALID_HEADER_FIELD);
        }

        int err = 0;
        err |= aws_add_size_checked(header.name.len, total, &total);
        err |= aws_add_size_checked(header.value.len, total, &total);
        err |= aws_add_size_checked(4, total, &total); /* ": " + "\r\n" */
        if (err) {
            return AWS_OP_ERR;
        }
    }
    if (aws_add_size_checked(2, total, &total)) { /* "\r\n" */
        return AWS_OP_ERR;
    }
    *out_size = total;
    return AWS_OP_SUCCESS;
}

static bool s_write_crlf(struct aws_byte_buf *dst) {
    AWS_PRECONDITION(aws_byte_buf_is_valid(dst));
    struct aws_byte_cursor crlf_cursor = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("\r\n");
    return aws_byte_buf_write_from_whole_cursor(dst, crlf_cursor);
}

static void s_write_headers(struct aws_byte_buf *dst, const struct aws_http_headers *headers) {

    const size_t num_headers = aws_http_headers_count(headers);

    bool wrote_all = true;
    for (size_t i = 0; i < num_headers; ++i) {
        struct aws_http_header header;
        aws_http_headers_get_index(headers, i, &header);

        /* header-line: "{name}: {value}\r\n" */
        wrote_all &= aws_byte_buf_write_from_whole_cursor(dst, header.name);
        wrote_all &= aws_byte_buf_write_u8(dst, ':');
        wrote_all &= aws_byte_buf_write_u8(dst, ' ');
        wrote_all &= aws_byte_buf_write_from_whole_cursor(dst, header.value);
        wrote_all &= s_write_crlf(dst);
    }
    AWS_ASSERT(wrote_all);
    (void)wrote_all;
}

int aws_h1_encoder_message_init_from_request(
    struct aws_h1_encoder_message *message,
    struct aws_allocator *allocator,
    const struct aws_http_message *request,
    struct aws_linked_list *pending_chunk_list) {

    AWS_PRECONDITION(aws_linked_list_is_valid(pending_chunk_list));

    AWS_ZERO_STRUCT(*message);

    message->body = aws_input_stream_acquire(aws_http_message_get_body_stream(request));
    message->pending_chunk_list = pending_chunk_list;

    struct aws_byte_cursor method;
    int err = aws_http_message_get_request_method(request, &method);
    if (err) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Request method not set");
        aws_raise_error(AWS_ERROR_HTTP_INVALID_METHOD);
        goto error;
    }
    /* RFC-7230 3.1.1: method = token */
    if (!aws_strutil_is_http_token(method)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Request method is invalid");
        aws_raise_error(AWS_ERROR_HTTP_INVALID_METHOD);
        goto error;
    }

    struct aws_byte_cursor uri;
    err = aws_http_message_get_request_path(request, &uri);
    if (err) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Request path not set");
        aws_raise_error(AWS_ERROR_HTTP_INVALID_PATH);
        goto error;
    }
    if (!aws_strutil_is_http_request_target(uri)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=static: Request path is invalid");
        aws_raise_error(AWS_ERROR_HTTP_INVALID_PATH);
        goto error;
    }

    struct aws_byte_cursor version = aws_http_version_to_str(AWS_HTTP_VERSION_1_1);

    /**
     * Calculate total size needed for outgoing_head_buffer, then write to buffer.
     */

    size_t header_lines_len;
    err = s_scan_outgoing_headers(
        message, request, &header_lines_len, false /*body_headers_ignored*/, false /*body_headers_forbidden*/);
    if (err) {
        goto error;
    }

    /* request-line: "{method} {uri} {version}\r\n" */
    size_t request_line_len = 4; /* 2 spaces + "\r\n" */
    err |= aws_add_size_checked(method.len, request_line_len, &request_line_len);
    err |= aws_add_size_checked(uri.len, request_line_len, &request_line_len);
    err |= aws_add_size_checked(version.len, request_line_len, &request_line_len);

    /* head-end: "\r\n" */
    size_t head_end_len = 2;

    size_t head_total_len = request_line_len;
    err |= aws_add_size_checked(header_lines_len, head_total_len, &head_total_len);
    err |= aws_add_size_checked(head_end_len, head_total_len, &head_total_len);
    if (err) {
        goto error;
    }

    err = aws_byte_buf_init(&message->outgoing_head_buf, allocator, head_total_len);
    if (err) {
        goto error;
    }

    bool wrote_all = true;

    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, method);
    wrote_all &= aws_byte_buf_write_u8(&message->outgoing_head_buf, ' ');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, uri);
    wrote_all &= aws_byte_buf_write_u8(&message->outgoing_head_buf, ' ');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, version);
    wrote_all &= s_write_crlf(&message->outgoing_head_buf);

    s_write_headers(&message->outgoing_head_buf, aws_http_message_get_const_headers(request));

    wrote_all &= s_write_crlf(&message->outgoing_head_buf);
    (void)wrote_all;
    AWS_ASSERT(wrote_all);

    return AWS_OP_SUCCESS;
error:
    aws_h1_encoder_message_clean_up(message);
    return AWS_OP_ERR;
}

int aws_h1_encoder_message_init_from_response(
    struct aws_h1_encoder_message *message,
    struct aws_allocator *allocator,
    const struct aws_http_message *response,
    bool body_headers_ignored,
    struct aws_linked_list *pending_chunk_list) {

    AWS_PRECONDITION(aws_linked_list_is_valid(pending_chunk_list));

    AWS_ZERO_STRUCT(*message);

    message->body = aws_input_stream_acquire(aws_http_message_get_body_stream(response));
    message->pending_chunk_list = pending_chunk_list;

    struct aws_byte_cursor version = aws_http_version_to_str(AWS_HTTP_VERSION_1_1);

    int status_int;
    int err = aws_http_message_get_response_status(response, &status_int);
    if (err) {
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_STATUS_CODE);
    }

    /* Status code must fit in 3 digits */
    AWS_ASSERT(status_int >= 0 && status_int <= 999); /* aws_http_message should have already checked this */
    char status_code_str[4] = "XXX";
    snprintf(status_code_str, sizeof(status_code_str), "%03d", status_int);
    struct aws_byte_cursor status_code = aws_byte_cursor_from_c_str(status_code_str);

    struct aws_byte_cursor status_text = aws_byte_cursor_from_c_str(aws_http_status_text(status_int));

    /**
     * Calculate total size needed for outgoing_head_buffer, then write to buffer.
     */

    size_t header_lines_len;
    /**
     * no body needed in the response
     * RFC-7230 section 3.3 Message Body
     */
    body_headers_ignored |= status_int == AWS_HTTP_STATUS_CODE_304_NOT_MODIFIED;
    bool body_headers_forbidden = status_int == AWS_HTTP_STATUS_CODE_204_NO_CONTENT || status_int / 100 == 1;
    err = s_scan_outgoing_headers(message, response, &header_lines_len, body_headers_ignored, body_headers_forbidden);
    if (err) {
        goto error;
    }

    /* valid status must be three digital code, change it into byte_cursor */
    /* response-line: "{version} {status} {status_text}\r\n" */
    size_t response_line_len = 4; /* 2 spaces + "\r\n" */
    err |= aws_add_size_checked(version.len, response_line_len, &response_line_len);
    err |= aws_add_size_checked(status_code.len, response_line_len, &response_line_len);
    err |= aws_add_size_checked(status_text.len, response_line_len, &response_line_len);

    /* head-end: "\r\n" */
    size_t head_end_len = 2;
    size_t head_total_len = response_line_len;
    err |= aws_add_size_checked(header_lines_len, head_total_len, &head_total_len);
    err |= aws_add_size_checked(head_end_len, head_total_len, &head_total_len);
    if (err) {
        goto error;
    }

    aws_byte_buf_init(&message->outgoing_head_buf, allocator, head_total_len);

    bool wrote_all = true;

    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, version);
    wrote_all &= aws_byte_buf_write_u8(&message->outgoing_head_buf, ' ');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, status_code);
    wrote_all &= aws_byte_buf_write_u8(&message->outgoing_head_buf, ' ');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(&message->outgoing_head_buf, status_text);
    wrote_all &= s_write_crlf(&message->outgoing_head_buf);

    s_write_headers(&message->outgoing_head_buf, aws_http_message_get_const_headers(response));

    wrote_all &= s_write_crlf(&message->outgoing_head_buf);
    (void)wrote_all;
    AWS_ASSERT(wrote_all);

    /* Success! */
    return AWS_OP_SUCCESS;

error:
    aws_h1_encoder_message_clean_up(message);
    return AWS_OP_ERR;
}

void aws_h1_encoder_message_clean_up(struct aws_h1_encoder_message *message) {
    aws_input_stream_release(message->body);
    aws_byte_buf_clean_up(&message->outgoing_head_buf);
    aws_h1_trailer_destroy(message->trailer);
    AWS_ZERO_STRUCT(*message);
}

void aws_h1_encoder_init(struct aws_h1_encoder *encoder, struct aws_allocator *allocator) {
    AWS_ZERO_STRUCT(*encoder);
    encoder->allocator = allocator;
}

void aws_h1_encoder_clean_up(struct aws_h1_encoder *encoder) {
    AWS_ZERO_STRUCT(*encoder);
}

int aws_h1_encoder_start_message(
    struct aws_h1_encoder *encoder,
    struct aws_h1_encoder_message *message,
    struct aws_http_stream *stream) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(message);

    if (encoder->message) {
        ENCODER_LOG(ERROR, encoder, "Attempting to start new request while previous request is in progress.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    encoder->current_stream = stream;
    encoder->message = message;

    return AWS_OP_SUCCESS;
}

static bool s_write_chunk_size(struct aws_byte_buf *dst, uint64_t chunk_size) {
    AWS_PRECONDITION(dst);
    AWS_PRECONDITION(aws_byte_buf_is_valid(dst));
    char ascii_hex_chunk_size_str[MAX_ASCII_HEX_CHUNK_STR_SIZE] = {0};
    snprintf(ascii_hex_chunk_size_str, sizeof(ascii_hex_chunk_size_str), "%" PRIX64, chunk_size);
    return aws_byte_buf_write_from_whole_cursor(dst, aws_byte_cursor_from_c_str(ascii_hex_chunk_size_str));
}

static bool s_write_chunk_extension(struct aws_byte_buf *dst, struct aws_http1_chunk_extension *chunk_extension) {
    AWS_PRECONDITION(chunk_extension);
    AWS_PRECONDITION(aws_byte_buf_is_valid(dst));
    bool wrote_all = true;
    wrote_all &= aws_byte_buf_write_u8(dst, ';');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(dst, chunk_extension->key);
    wrote_all &= aws_byte_buf_write_u8(dst, '=');
    wrote_all &= aws_byte_buf_write_from_whole_cursor(dst, chunk_extension->value);
    return wrote_all;
}

static size_t s_calculate_chunk_line_size(const struct aws_http1_chunk_options *options) {
    size_t chunk_line_size = MAX_ASCII_HEX_CHUNK_STR_SIZE + CRLF_SIZE;
    for (size_t i = 0; i < options->num_extensions; ++i) {
        struct aws_http1_chunk_extension *chunk_extension = options->extensions + i;
        chunk_line_size += 1 /* ; */;
        chunk_line_size += chunk_extension->key.len;
        chunk_line_size += 1 /* = */;
        chunk_line_size += chunk_extension->value.len;
    }
    return chunk_line_size;
}

static void s_populate_chunk_line_buffer(
    struct aws_byte_buf *chunk_line,
    const struct aws_http1_chunk_options *options) {

    bool wrote_chunk_line = true;
    wrote_chunk_line &= s_write_chunk_size(chunk_line, options->chunk_data_size);
    for (size_t i = 0; i < options->num_extensions; ++i) {
        wrote_chunk_line &= s_write_chunk_extension(chunk_line, options->extensions + i);
    }
    wrote_chunk_line &= s_write_crlf(chunk_line);
    AWS_ASSERT(wrote_chunk_line);
    (void)wrote_chunk_line;
}

struct aws_h1_trailer *aws_h1_trailer_new(
    struct aws_allocator *allocator,
    const struct aws_http_headers *trailing_headers) {
    /* Allocate trailer along with storage for the trailer-line */
    size_t trailer_size = 0;
    if (s_scan_outgoing_trailer(trailing_headers, &trailer_size)) {
        return NULL;
    }

    struct aws_h1_trailer *trailer = aws_mem_calloc(allocator, 1, sizeof(struct aws_h1_trailer));
    trailer->allocator = allocator;

    aws_byte_buf_init(&trailer->trailer_data, allocator, trailer_size); /* cannot fail */
    s_write_headers(&trailer->trailer_data, trailing_headers);
    s_write_crlf(&trailer->trailer_data); /* \r\n */
    return trailer;
}

void aws_h1_trailer_destroy(struct aws_h1_trailer *trailer) {
    if (trailer == NULL) {
        return;
    }
    aws_byte_buf_clean_up(&trailer->trailer_data);
    aws_mem_release(trailer->allocator, trailer);
}

struct aws_h1_chunk *aws_h1_chunk_new(struct aws_allocator *allocator, const struct aws_http1_chunk_options *options) {
    /* Allocate chunk along with storage for the chunk-line */
    struct aws_h1_chunk *chunk;
    size_t chunk_line_size = s_calculate_chunk_line_size(options);
    void *chunk_line_storage;
    if (!aws_mem_acquire_many(
            allocator, 2, &chunk, sizeof(struct aws_h1_chunk), &chunk_line_storage, chunk_line_size)) {
        return NULL;
    }

    chunk->allocator = allocator;
    chunk->data = aws_input_stream_acquire(options->chunk_data);
    chunk->data_size = options->chunk_data_size;
    chunk->on_complete = options->on_complete;
    chunk->user_data = options->user_data;
    chunk->chunk_line = aws_byte_buf_from_empty_array(chunk_line_storage, chunk_line_size);
    s_populate_chunk_line_buffer(&chunk->chunk_line, options);
    return chunk;
}

void aws_h1_chunk_destroy(struct aws_h1_chunk *chunk) {
    AWS_PRECONDITION(chunk);
    aws_input_stream_release(chunk->data);
    aws_mem_release(chunk->allocator, chunk);
}

void aws_h1_chunk_complete_and_destroy(
    struct aws_h1_chunk *chunk,
    struct aws_http_stream *http_stream,
    int error_code) {

    AWS_PRECONDITION(chunk);

    aws_http1_stream_write_chunk_complete_fn *on_complete = chunk->on_complete;
    void *user_data = chunk->user_data;

    /* Clean up before firing callback */
    aws_h1_chunk_destroy(chunk);

    if (NULL != on_complete) {
        on_complete(http_stream, error_code, user_data);
    }
}

static void s_clean_up_current_chunk(struct aws_h1_encoder *encoder, int error_code) {
    AWS_PRECONDITION(encoder->current_chunk);
    AWS_PRECONDITION(&encoder->current_chunk->node == aws_linked_list_front(encoder->message->pending_chunk_list));

    aws_linked_list_remove(&encoder->current_chunk->node);
    aws_h1_chunk_complete_and_destroy(encoder->current_chunk, encoder->current_stream, error_code);
    encoder->current_chunk = NULL;
}

/* Write as much as possible from src_buf to dst, using encoder->progress_len to track progress.
 * Returns true if the entire src_buf has been copied */
static bool s_encode_buf(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst, const struct aws_byte_buf *src) {

    /* advance src_cursor to current position in src_buf */
    struct aws_byte_cursor src_cursor = aws_byte_cursor_from_buf(src);
    aws_byte_cursor_advance(&src_cursor, (size_t)encoder->progress_bytes);

    /* write as much as possible to dst, src_cursor is advanced as write occurs */
    struct aws_byte_cursor written = aws_byte_buf_write_to_capacity(dst, &src_cursor);
    encoder->progress_bytes += written.len;

    return src_cursor.len == 0;
}

/* Write as much body stream as possible into dst buffer.
 * Increments encoder->progress_bytes to track progress */
static int s_encode_stream(
    struct aws_h1_encoder *encoder,
    struct aws_byte_buf *dst,
    struct aws_input_stream *stream,
    uint64_t total_length,
    bool *out_done) {

    *out_done = false;

    if (dst->capacity == dst->len) {
        /* Return success because we want to try again later */
        return AWS_OP_SUCCESS;
    }

    /* Read from stream */
    ENCODER_LOG(TRACE, encoder, "Reading from body stream.");
    const size_t prev_len = dst->len;
    int err = aws_input_stream_read(stream, dst);
    const size_t amount_read = dst->len - prev_len;

    if (err) {
        ENCODER_LOGF(
            ERROR,
            encoder,
            "Failed to read body stream, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        return AWS_OP_ERR;
    }

    /* Increment progress_bytes, and make sure we haven't written too much */
    int add_err = aws_add_u64_checked(encoder->progress_bytes, amount_read, &encoder->progress_bytes);
    if (add_err || encoder->progress_bytes > total_length) {
        ENCODER_LOGF(ERROR, encoder, "Body stream has exceeded expected length: %" PRIu64, total_length);
        return aws_raise_error(AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT);
    }

    ENCODER_LOGF(
        TRACE,
        encoder,
        "Sending %zu bytes of body, progress: %" PRIu64 "/%" PRIu64,
        amount_read,
        encoder->progress_bytes,
        total_length);

    /* Return if we're done sending stream */
    if (encoder->progress_bytes == total_length) {
        *out_done = true;
        return AWS_OP_SUCCESS;
    }

    /* Return if stream failed to write anything. Maybe the data isn't ready yet. */
    if (amount_read == 0) {
        /* Ensure we're not at end-of-stream too early */
        struct aws_stream_status status;
        err = aws_input_stream_get_status(stream, &status);
        if (err) {
            ENCODER_LOGF(
                TRACE,
                encoder,
                "Failed to query body stream status, error %d (%s)",
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
        if (status.is_end_of_stream) {
            ENCODER_LOGF(
                ERROR,
                encoder,
                "Reached end of body stream but sent less than declared length %" PRIu64 "/%" PRIu64,
                encoder->progress_bytes,
                total_length);
            return aws_raise_error(AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT);
        }
    }

    /* Not done streaming data out yet */
    return AWS_OP_SUCCESS;
}

/* A state function should:
 * - Raise an error only if unrecoverable error occurs.
 * - `return s_switch_state(...)` to switch states.
 * - `return AWS_OP_SUCCESS` if it can't progress any further (waiting for more
 *    space to write into, waiting for more chunks, etc). */
typedef int encoder_state_fn(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst);

/* Switch state.
 * The only reason this returns a value is so it can be called with `return` to conclude a state function */
static int s_switch_state(struct aws_h1_encoder *encoder, enum aws_h1_encoder_state state) {
    encoder->state = state;
    encoder->progress_bytes = 0;
    return AWS_OP_SUCCESS;
}

/* Initial state. Waits until a new message is set */
static int s_state_fn_init(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    (void)dst;

    if (!encoder->message) {
        /* Remain in this state. */
        return AWS_OP_SUCCESS;
    }

    /* Start encoding message */
    ENCODER_LOG(TRACE, encoder, "Starting to send data.");
    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_HEAD);
}

/* Write out first line of request/response, plus all the headers.
 * These have been pre-encoded in aws_h1_encoder_message->outgoing_head_buf. */
static int s_state_fn_head(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done = s_encode_buf(encoder, dst, &encoder->message->outgoing_head_buf);
    if (!done) {
        /* Remain in this state */
        return AWS_OP_SUCCESS;
    }

    /* Don't NEED to free this buffer now, but we don't need it anymore, so why not */
    aws_byte_buf_clean_up(&encoder->message->outgoing_head_buf);

    /* Pick next state */
    if (encoder->message->body && encoder->message->content_length) {
        return s_switch_state(encoder, AWS_H1_ENCODER_STATE_UNCHUNKED_BODY);

    } else if (encoder->message->has_chunked_encoding_header) {
        return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_NEXT);

    } else {
        return s_switch_state(encoder, AWS_H1_ENCODER_STATE_DONE);
    }
}

/* Write out body (not using chunked encoding). */
static int s_state_fn_unchunked_body(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done;
    if (s_encode_stream(encoder, dst, encoder->message->body, encoder->message->content_length, &done)) {
        return AWS_OP_ERR;
    }

    if (!done) {
        /* Remain in this state until we're done writing out body */
        return AWS_OP_SUCCESS;
    }

    /* Message is done */
    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_DONE);
}

/* Select next chunk to work on.
 * Encoder is essentially "paused" here if no chunks are available. */
static int s_state_fn_chunk_next(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    (void)dst;

    if (aws_linked_list_empty(encoder->message->pending_chunk_list)) {
        /* Remain in this state until more chunks arrive */
        ENCODER_LOG(TRACE, encoder, "No chunks ready to send, waiting for more...");
        return AWS_OP_SUCCESS;
    }

    /* Set next chunk and go to next state */
    struct aws_linked_list_node *node = aws_linked_list_front(encoder->message->pending_chunk_list);
    encoder->current_chunk = AWS_CONTAINER_OF(node, struct aws_h1_chunk, node);
    encoder->chunk_count++;
    ENCODER_LOGF(
        TRACE,
        encoder,
        "Begin sending chunk %zu with size %" PRIu64,
        encoder->chunk_count,
        encoder->current_chunk->data_size);

    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_LINE);
}

/* Write out "chunk-size [chunk-ext] CRLF".
 * This data is pre-encoded in the chunk's chunk_line buffer */
static int s_state_fn_chunk_line(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done = s_encode_buf(encoder, dst, &encoder->current_chunk->chunk_line);
    if (!done) {
        /* Remain in state until done writing line */
        return AWS_OP_SUCCESS;
    }

    /* Pick next state */
    if (encoder->current_chunk->data_size == 0) {
        /* If data_size is 0, then this was the last chunk, which has no body.
         * Mark it complete and move on to trailer. */
        ENCODER_LOG(TRACE, encoder, "Final chunk complete");
        s_clean_up_current_chunk(encoder, AWS_ERROR_SUCCESS);

        return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_TRAILER);
    }

    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_BODY);
}

/* Write out data for current chunk */
static int s_state_fn_chunk_body(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done;
    if (s_encode_stream(encoder, dst, encoder->current_chunk->data, encoder->current_chunk->data_size, &done)) {
        int error_code = aws_last_error();

        /* The error was caused by the chunk itself, report that specific error in its completion callback */
        s_clean_up_current_chunk(encoder, error_code);

        /* Re-raise error, in case it got cleared during user callback */
        return aws_raise_error(error_code);
    }
    if (!done) {
        /* Remain in this state until we're done writing out body */
        return AWS_OP_SUCCESS;
    }

    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_END);
}

/* Write CRLF and mark chunk as complete */
static int s_state_fn_chunk_end(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done = s_write_crlf(dst);
    if (!done) {
        /* Remain in this state until done writing out CRLF */
        return AWS_OP_SUCCESS;
    }

    ENCODER_LOG(TRACE, encoder, "Chunk complete");
    s_clean_up_current_chunk(encoder, AWS_ERROR_SUCCESS);

    /* Pick next chunk to work on */
    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_CHUNK_NEXT);
}

/* Write out trailer after last chunk */
static int s_state_fn_chunk_trailer(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    bool done;
    /* if a chunked trailer was set */
    if (encoder->message->trailer) {
        done = s_encode_buf(encoder, dst, &encoder->message->trailer->trailer_data);
    } else {
        done = s_write_crlf(dst);
    }
    if (!done) {
        /* Remain in this state until we're done writing out trailer */
        return AWS_OP_SUCCESS;
    }

    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_DONE);
}

/* Message is done, loop back to start of state machine */
static int s_state_fn_done(struct aws_h1_encoder *encoder, struct aws_byte_buf *dst) {
    (void)dst;

    ENCODER_LOG(TRACE, encoder, "Done sending data.");
    encoder->message = NULL;
    return s_switch_state(encoder, AWS_H1_ENCODER_STATE_INIT);
}

struct encoder_state_def {
    encoder_state_fn *fn;
    const char *name;
};

static struct encoder_state_def s_encoder_states[] = {
    [AWS_H1_ENCODER_STATE_INIT] = {.fn = s_state_fn_init, .name = "INIT"},
    [AWS_H1_ENCODER_STATE_HEAD] = {.fn = s_state_fn_head, .name = "HEAD"},
    [AWS_H1_ENCODER_STATE_UNCHUNKED_BODY] = {.fn = s_state_fn_unchunked_body, .name = "BODY"},
    [AWS_H1_ENCODER_STATE_CHUNK_NEXT] = {.fn = s_state_fn_chunk_next, .name = "CHUNK_NEXT"},
    [AWS_H1_ENCODER_STATE_CHUNK_LINE] = {.fn = s_state_fn_chunk_line, .name = "CHUNK_LINE"},
    [AWS_H1_ENCODER_STATE_CHUNK_BODY] = {.fn = s_state_fn_chunk_body, .name = "CHUNK_BODY"},
    [AWS_H1_ENCODER_STATE_CHUNK_END] = {.fn = s_state_fn_chunk_end, .name = "CHUNK_END"},
    [AWS_H1_ENCODER_STATE_CHUNK_TRAILER] = {.fn = s_state_fn_chunk_trailer, .name = "CHUNK_TRAILER"},
    [AWS_H1_ENCODER_STATE_DONE] = {.fn = s_state_fn_done, .name = "DONE"},
};

int aws_h1_encoder_process(struct aws_h1_encoder *encoder, struct aws_byte_buf *out_buf) {
    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(out_buf);

    if (!encoder->message) {
        ENCODER_LOG(ERROR, encoder, "No message is currently set for encoding.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* Run state machine until states stop changing. (due to out_buf running
     * out of space, input_stream stalling, waiting for more chunks, etc) */
    enum aws_h1_encoder_state prev_state;
    do {
        prev_state = encoder->state;
        if (s_encoder_states[encoder->state].fn(encoder, out_buf)) {
            return AWS_OP_ERR;
        }
    } while (prev_state != encoder->state);

    return AWS_OP_SUCCESS;
}

bool aws_h1_encoder_is_message_in_progress(const struct aws_h1_encoder *encoder) {
    return encoder->message;
}

bool aws_h1_encoder_is_waiting_for_chunks(const struct aws_h1_encoder *encoder) {
    return encoder->state == AWS_H1_ENCODER_STATE_CHUNK_NEXT &&
           aws_linked_list_empty(encoder->message->pending_chunk_list);
}
