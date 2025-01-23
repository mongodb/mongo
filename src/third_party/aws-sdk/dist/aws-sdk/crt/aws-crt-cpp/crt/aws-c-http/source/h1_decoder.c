/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/string.h>
#include <aws/http/private/h1_decoder.h>
#include <aws/http/private/strutil.h>
#include <aws/http/status_code.h>
#include <aws/io/logging.h>

AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_chunked, "chunked");
AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_compress, "compress");
AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_x_compress, "x-compress");
AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_deflate, "deflate");
AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_gzip, "gzip");
AWS_STATIC_STRING_FROM_LITERAL(s_transfer_coding_x_gzip, "x-gzip");

/* Decoder runs a state machine.
 * Each state consumes data until it sets the next state.
 * A common state is the "line state", which handles consuming one line ending in CRLF
 * and feeding the line to a linestate_fn, which should process data and set the next state.
 */
typedef int(state_fn)(struct aws_h1_decoder *decoder, struct aws_byte_cursor *input);
typedef int(linestate_fn)(struct aws_h1_decoder *decoder, struct aws_byte_cursor input);

struct aws_h1_decoder {
    /* Implementation data. */
    struct aws_allocator *alloc;
    struct aws_byte_buf scratch_space;
    state_fn *run_state;
    linestate_fn *process_line;
    int transfer_encoding;
    uint64_t content_processed;
    uint64_t content_length;
    uint64_t chunk_processed;
    uint64_t chunk_size;
    bool doing_trailers;
    bool is_done;
    bool body_headers_ignored;
    bool body_headers_forbidden;
    enum aws_http_header_block header_block;
    const void *logging_id;

    /* User callbacks and settings. */
    struct aws_h1_decoder_vtable vtable;
    bool is_decoding_requests;
    void *user_data;
};

static int s_linestate_request(struct aws_h1_decoder *decoder, struct aws_byte_cursor input);
static int s_linestate_response(struct aws_h1_decoder *decoder, struct aws_byte_cursor input);
static int s_linestate_header(struct aws_h1_decoder *decoder, struct aws_byte_cursor input);
static int s_linestate_chunk_size(struct aws_h1_decoder *decoder, struct aws_byte_cursor input);

static bool s_scan_for_crlf(struct aws_h1_decoder *decoder, struct aws_byte_cursor input, size_t *bytes_processed) {
    AWS_ASSERT(input.len > 0);

    /* In a loop, scan for "\n", then look one char back for "\r" */
    uint8_t *ptr = input.ptr;
    uint8_t *end = input.ptr + input.len;
    while (ptr != end) {
        uint8_t *newline = (uint8_t *)memchr(ptr, '\n', end - ptr);
        if (!newline) {
            break;
        }

        uint8_t prev_char;
        if (newline == input.ptr) {
            /* If "\n" is first character check scratch_space for previous character */
            if (decoder->scratch_space.len > 0) {
                prev_char = decoder->scratch_space.buffer[decoder->scratch_space.len - 1];
            } else {
                prev_char = 0;
            }
        } else {
            prev_char = *(newline - 1);
        }

        if (prev_char == '\r') {
            *bytes_processed = 1 + (newline - input.ptr);
            return true;
        }

        ptr = newline + 1;
    }

    *bytes_processed = input.len;
    return false;
}

/* This state consumes an entire line, then calls a linestate_fn to process the line. */
static int s_state_getline(struct aws_h1_decoder *decoder, struct aws_byte_cursor *input) {
    /* If preceding runs of this state failed to find CRLF, their data is stored in the scratch_space
     * and new data needs to be combined with the old data for processing. */
    bool has_prev_data = decoder->scratch_space.len;

    size_t line_length = 0;
    bool found_crlf = s_scan_for_crlf(decoder, *input, &line_length);

    /* Found end of line! Run the line processor on it */
    struct aws_byte_cursor line = aws_byte_cursor_advance(input, line_length);

    bool use_scratch = !found_crlf | has_prev_data;
    if (AWS_UNLIKELY(use_scratch)) {
        if (aws_byte_buf_append_dynamic(&decoder->scratch_space, &line)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Internal buffer write failed with error code %d (%s)",
                decoder->logging_id,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
        /* Line is actually the entire scratch buffer now */
        line = aws_byte_cursor_from_buf(&decoder->scratch_space);
    }

    if (AWS_LIKELY(found_crlf)) {
        /* Backup so "\r\n" is not included. */
        /* RFC-7230 section 3 Message Format */
        AWS_ASSERT(line.len >= 2);
        line.len -= 2;

        return decoder->process_line(decoder, line);
    }

    /* Didn't find crlf, we'll continue scanning when more data comes in */
    return AWS_OP_SUCCESS;
}

static int s_cursor_split_impl(
    struct aws_byte_cursor input,
    char split_on,
    struct aws_byte_cursor *cursor_array,
    size_t num_cursors,
    bool error_if_more_splits_possible) {

    struct aws_byte_cursor split;
    AWS_ZERO_STRUCT(split);
    for (size_t i = 0; i < num_cursors; ++i) {
        if (!aws_byte_cursor_next_split(&input, split_on, &split)) {
            return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
        }
        cursor_array[i] = split;
    }

    if (error_if_more_splits_possible) {
        if (aws_byte_cursor_next_split(&input, split_on, &split)) {
            return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
        }
    } else {
        /* Otherwise, the last cursor will contain the remainder of the string */
        struct aws_byte_cursor *last_cursor = &cursor_array[num_cursors - 1];
        last_cursor->len = (input.ptr + input.len) - last_cursor->ptr;
    }

    return AWS_OP_SUCCESS;
}

/* Final cursor contains remainder of input. */
static int s_cursor_split_first_n_times(
    struct aws_byte_cursor input,
    char split_on,
    struct aws_byte_cursor *cursor_array,
    size_t num_cursors) {

    return s_cursor_split_impl(input, split_on, cursor_array, num_cursors, false);
}

/* Error if input could have been split more times */
static int s_cursor_split_exactly_n_times(
    struct aws_byte_cursor input,
    char split_on,
    struct aws_byte_cursor *cursor_array,
    size_t num_cursors) {

    return s_cursor_split_impl(input, split_on, cursor_array, num_cursors, true);
}

static void s_set_state(struct aws_h1_decoder *decoder, state_fn *state) {
    decoder->scratch_space.len = 0;
    decoder->run_state = state;
    decoder->process_line = NULL;
}

/* Set next state to capture a full line, then call the specified linestate_fn on it */
static void s_set_line_state(struct aws_h1_decoder *decoder, linestate_fn *line_processor) {
    s_set_state(decoder, s_state_getline);
    decoder->process_line = line_processor;
}

static int s_mark_done(struct aws_h1_decoder *decoder) {
    decoder->is_done = true;

    return decoder->vtable.on_done(decoder->user_data);
}

/* Reset state, in preparation for processing a new message */
static void s_reset_state(struct aws_h1_decoder *decoder) {
    if (decoder->is_decoding_requests) {
        s_set_line_state(decoder, s_linestate_request);
    } else {
        s_set_line_state(decoder, s_linestate_response);
    }

    decoder->transfer_encoding = 0;
    decoder->content_processed = 0;
    decoder->content_length = 0;
    decoder->chunk_processed = 0;
    decoder->chunk_size = 0;
    decoder->doing_trailers = false;
    decoder->is_done = false;
    decoder->body_headers_ignored = false;
    decoder->body_headers_forbidden = false;
    /* set to normal by default */
    decoder->header_block = AWS_HTTP_HEADER_BLOCK_MAIN;
}

static int s_state_unchunked_body(struct aws_h1_decoder *decoder, struct aws_byte_cursor *input) {

    size_t processed_bytes = 0;
    AWS_FATAL_ASSERT(decoder->content_processed < decoder->content_length); /* shouldn't be possible */

    if (input->len > (decoder->content_length - decoder->content_processed)) {
        processed_bytes = (size_t)(decoder->content_length - decoder->content_processed);
    } else {
        processed_bytes = input->len;
    }

    decoder->content_processed += processed_bytes;

    bool finished = decoder->content_processed == decoder->content_length;
    struct aws_byte_cursor body = aws_byte_cursor_advance(input, processed_bytes);
    int err = decoder->vtable.on_body(&body, finished, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    if (AWS_LIKELY(finished)) {
        err = s_mark_done(decoder);
        if (err) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_linestate_chunk_terminator(struct aws_h1_decoder *decoder, struct aws_byte_cursor input) {

    /* Expecting CRLF at end of each chunk */
    /* RFC-7230 section 4.1 Chunked Transfer Encoding */
    if (AWS_UNLIKELY(input.len != 0)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Incoming chunk is invalid, does not end with CRLF.", decoder->logging_id);
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    s_set_line_state(decoder, s_linestate_chunk_size);

    return AWS_OP_SUCCESS;
}

static int s_state_chunk(struct aws_h1_decoder *decoder, struct aws_byte_cursor *input) {
    size_t processed_bytes = 0;
    AWS_ASSERT(decoder->chunk_processed < decoder->chunk_size);

    if (input->len > (decoder->chunk_size - decoder->chunk_processed)) {
        processed_bytes = (size_t)(decoder->chunk_size - decoder->chunk_processed);
    } else {
        processed_bytes = input->len;
    }

    decoder->chunk_processed += processed_bytes;

    bool finished = decoder->chunk_processed == decoder->chunk_size;
    struct aws_byte_cursor body = aws_byte_cursor_advance(input, processed_bytes);
    int err = decoder->vtable.on_body(&body, false, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    if (AWS_LIKELY(finished)) {
        s_set_line_state(decoder, s_linestate_chunk_terminator);
    }

    return AWS_OP_SUCCESS;
}

static int s_linestate_chunk_size(struct aws_h1_decoder *decoder, struct aws_byte_cursor input) {
    struct aws_byte_cursor size;
    AWS_ZERO_STRUCT(size);
    if (!aws_byte_cursor_next_split(&input, ';', &size)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Incoming chunk is invalid, first line is malformed.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad chunk line is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(input));

        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    int err = aws_byte_cursor_utf8_parse_u64_hex(size, &decoder->chunk_size);
    if (err) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Failed to parse size of incoming chunk.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad chunk size is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(size));

        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }
    decoder->chunk_processed = 0;

    /* Empty chunk signifies all chunks have been read. */
    if (AWS_UNLIKELY(decoder->chunk_size == 0)) {
        struct aws_byte_cursor cursor;
        cursor.ptr = NULL;
        cursor.len = 0;
        err = decoder->vtable.on_body(&cursor, true, decoder->user_data);
        if (err) {
            return AWS_OP_ERR;
        }

        /* Expected empty newline and end of message. */
        decoder->doing_trailers = true;
        s_set_line_state(decoder, s_linestate_header);
        return AWS_OP_SUCCESS;
    }

    /* Skip all chunk extensions, as they are optional. */
    /* RFC-7230 section 4.1.1 Chunk Extensions */

    s_set_state(decoder, s_state_chunk);

    return AWS_OP_SUCCESS;
}

static int s_linestate_header(struct aws_h1_decoder *decoder, struct aws_byte_cursor input) {
    int err;

    /* The \r\n was just processed by `s_state_getline`. */
    /* Empty line signifies end of headers, and beginning of body or end of trailers. */
    /* RFC-7230 section 3 Message Format */
    if (input.len == 0) {
        if (AWS_LIKELY(!decoder->doing_trailers)) {
            if (decoder->body_headers_ignored) {
                err = s_mark_done(decoder);
                if (err) {
                    return AWS_OP_ERR;
                }
            } else if (decoder->transfer_encoding & AWS_HTTP_TRANSFER_ENCODING_CHUNKED) {
                s_set_line_state(decoder, s_linestate_chunk_size);
            } else if (decoder->content_length > 0) {
                s_set_state(decoder, s_state_unchunked_body);
            } else {
                err = s_mark_done(decoder);
                if (err) {
                    return AWS_OP_ERR;
                }
            }
        } else {
            /* Empty line means end of message. */
            err = s_mark_done(decoder);
            if (err) {
                return AWS_OP_ERR;
            }
        }

        return AWS_OP_SUCCESS;
    }

    /* Each header field consists of a case-insensitive field name followed by a colon (":"),
     * optional leading whitespace, the field value, and optional trailing whitespace.
     * RFC-7230 3.2 */
    struct aws_byte_cursor splits[2];
    err = s_cursor_split_first_n_times(input, ':', splits, 2); /* value may contain more colons */
    if (err) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Invalid incoming header, missing colon.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM, "id=%p: Bad header is: '" PRInSTR "'", decoder->logging_id, AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    struct aws_byte_cursor name = splits[0];
    if (!aws_strutil_is_http_token(name)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Invalid incoming header, bad name.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM, "id=%p: Bad header is: '" PRInSTR "'", decoder->logging_id, AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    struct aws_byte_cursor value = aws_strutil_trim_http_whitespace(splits[1]);
    if (!aws_strutil_is_http_field_value(value)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Invalid incoming header, bad value.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM, "id=%p: Bad header is: '" PRInSTR "'", decoder->logging_id, AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    struct aws_h1_decoded_header header;
    header.name = aws_http_str_to_header_name(name);
    header.name_data = name;
    header.value_data = value;
    header.data = input;

    switch (header.name) {
        case AWS_HTTP_HEADER_CONTENT_LENGTH:
            if (decoder->transfer_encoding) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming headers for both content-length and transfer-encoding received. This is illegal.",
                    decoder->logging_id);
                return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
            }

            if (aws_byte_cursor_utf8_parse_u64(header.value_data, &decoder->content_length)) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming content-length header has invalid value.",
                    decoder->logging_id);
                AWS_LOGF_DEBUG(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Bad content-length value is: '" PRInSTR "'",
                    decoder->logging_id,
                    AWS_BYTE_CURSOR_PRI(header.value_data));
                return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
            }

            if (decoder->body_headers_forbidden && decoder->content_length != 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming headers for content-length received, but it is illegal for this message to have a "
                    "body",
                    decoder->logging_id);
                return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
            }

            break;

        case AWS_HTTP_HEADER_TRANSFER_ENCODING: {
            if (decoder->content_length) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming headers for both content-length and transfer-encoding received. This is illegal.",
                    decoder->logging_id);
                return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
            }

            if (decoder->body_headers_forbidden) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming headers for transfer-encoding received, but it is illegal for this message to "
                    "have a body",
                    decoder->logging_id);
                return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
            }
            /* RFC-7230 section 3.3.1 Transfer-Encoding */
            /* RFC-7230 section 4.2 Compression Codings */

            /* Note that it's possible for multiple Transfer-Encoding headers to exist, in which case the values
             * should be appended with those from any previously encountered Transfer-Encoding headers. */
            struct aws_byte_cursor split;
            AWS_ZERO_STRUCT(split);
            while (aws_byte_cursor_next_split(&header.value_data, ',', &split)) {
                struct aws_byte_cursor coding = aws_strutil_trim_http_whitespace(split);
                int prev_flags = decoder->transfer_encoding;

                if (aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_chunked, &coding)) {
                    decoder->transfer_encoding |= AWS_HTTP_TRANSFER_ENCODING_CHUNKED;

                } else if (
                    aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_compress, &coding) ||
                    aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_x_compress, &coding)) {
                    /* A recipient SHOULD consider "x-compress" to be equivalent to "compress". RFC-7230 4.2.1 */
                    decoder->transfer_encoding |= AWS_HTTP_TRANSFER_ENCODING_DEPRECATED_COMPRESS;

                } else if (aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_deflate, &coding)) {
                    decoder->transfer_encoding |= AWS_HTTP_TRANSFER_ENCODING_DEFLATE;

                } else if (
                    aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_gzip, &coding) ||
                    aws_string_eq_byte_cursor_ignore_case(s_transfer_coding_x_gzip, &coding)) {
                    /* A recipient SHOULD consider "x-gzip" to be equivalent to "gzip". RFC-7230 4.2.3 */
                    decoder->transfer_encoding |= AWS_HTTP_TRANSFER_ENCODING_GZIP;

                } else if (coding.len > 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_HTTP_STREAM,
                        "id=%p: Incoming transfer-encoding header lists unrecognized coding.",
                        decoder->logging_id);
                    AWS_LOGF_DEBUG(
                        AWS_LS_HTTP_STREAM,
                        "id=%p: Unrecognized coding is: '" PRInSTR "'",
                        decoder->logging_id,
                        AWS_BYTE_CURSOR_PRI(coding));
                    return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
                }

                /* If any transfer coding other than chunked is applied to a request payload body, the sender MUST
                 * apply chunked as the final transfer coding to ensure that the message is properly framed.
                 * RFC-7230 3.3.1 */
                if ((prev_flags & AWS_HTTP_TRANSFER_ENCODING_CHUNKED) && (decoder->transfer_encoding != prev_flags)) {
                    AWS_LOGF_ERROR(
                        AWS_LS_HTTP_STREAM,
                        "id=%p: Incoming transfer-encoding header lists a coding after 'chunked', this is illegal.",
                        decoder->logging_id);
                    AWS_LOGF_DEBUG(
                        AWS_LS_HTTP_STREAM,
                        "id=%p: Misplaced coding is '" PRInSTR "'",
                        decoder->logging_id,
                        AWS_BYTE_CURSOR_PRI(coding));
                    return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
                }
            }

            /* TODO: deal with body of indeterminate length, marking it as successful when connection is closed:
             *
             * A response that has neither chunked transfer coding nor Content-Length is terminated by closure of
             * the connection and, thus, is considered complete regardless of the number of message body octets
             * received, provided that the header section was received intact.
             * RFC-7230 3.4 */
        } break;

        default:
            break;
    }

    err = decoder->vtable.on_header(&header, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    s_set_line_state(decoder, s_linestate_header);

    return AWS_OP_SUCCESS;
}

static int s_linestate_request(struct aws_h1_decoder *decoder, struct aws_byte_cursor input) {
    struct aws_byte_cursor cursors[3];
    int err = s_cursor_split_exactly_n_times(input, ' ', cursors, 3); /* extra spaces not allowed */
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Incoming request line has wrong number of spaces.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad request line is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    for (size_t i = 0; i < AWS_ARRAY_SIZE(cursors); ++i) {
        if (cursors[i].len == 0) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming request line has empty values.", decoder->logging_id);
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_STREAM,
                "id=%p: Bad request line is: '" PRInSTR "'",
                decoder->logging_id,
                AWS_BYTE_CURSOR_PRI(input));
            return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
        }
    }

    struct aws_byte_cursor method = cursors[0];
    struct aws_byte_cursor uri = cursors[1];
    struct aws_byte_cursor version = cursors[2];

    if (!aws_strutil_is_http_token(method)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming request has invalid method.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad request line is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    if (!aws_strutil_is_http_request_target(uri)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming request has invalid path.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad request line is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    struct aws_byte_cursor version_expected = aws_http_version_to_str(AWS_HTTP_VERSION_1_1);
    if (!aws_byte_cursor_eq(&version, &version_expected)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Incoming request uses unsupported HTTP version.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Unsupported version is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(version));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    err = decoder->vtable.on_request(aws_http_str_to_method(method), &method, &uri, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    s_set_line_state(decoder, s_linestate_header);

    return AWS_OP_SUCCESS;
}

static bool s_check_info_response_status_code(int code_val) {
    return code_val >= 100 && code_val < 200;
}

static int s_linestate_response(struct aws_h1_decoder *decoder, struct aws_byte_cursor input) {
    struct aws_byte_cursor cursors[3];
    int err = s_cursor_split_first_n_times(input, ' ', cursors, 3); /* phrase may contain spaces */
    if (err) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming response status line is invalid.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad status line is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(input));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    struct aws_byte_cursor version = cursors[0];
    struct aws_byte_cursor code = cursors[1];
    struct aws_byte_cursor phrase = cursors[2];

    struct aws_byte_cursor version_1_1_expected = aws_http_version_to_str(AWS_HTTP_VERSION_1_1);
    struct aws_byte_cursor version_1_0_expected = aws_http_version_to_str(AWS_HTTP_VERSION_1_0);
    if (!aws_byte_cursor_eq(&version, &version_1_1_expected) && !aws_byte_cursor_eq(&version, &version_1_0_expected)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Incoming response uses unsupported HTTP version.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Unsupported version is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(version));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    /* Validate phrase */
    if (!aws_strutil_is_http_reason_phrase(phrase)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming response has invalid reason phrase.", decoder->logging_id);
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    /* Status-code is a 3-digit integer. RFC7230 section 3.1.2 */
    uint64_t code_val_u64;
    err = aws_byte_cursor_utf8_parse_u64(code, &code_val_u64);
    if (err || code.len != 3 || code_val_u64 > 999) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Incoming response has invalid status code.", decoder->logging_id);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Bad status code is: '" PRInSTR "'",
            decoder->logging_id,
            AWS_BYTE_CURSOR_PRI(code));
        return aws_raise_error(AWS_ERROR_HTTP_PROTOCOL_ERROR);
    }

    int code_val = (int)code_val_u64;

    /* RFC-7230 section 3.3 Message Body */
    decoder->body_headers_ignored |= code_val == AWS_HTTP_STATUS_CODE_304_NOT_MODIFIED;
    decoder->body_headers_forbidden = code_val == AWS_HTTP_STATUS_CODE_204_NO_CONTENT || code_val / 100 == 1;

    if (s_check_info_response_status_code(code_val)) {
        decoder->header_block = AWS_HTTP_HEADER_BLOCK_INFORMATIONAL;
    }

    err = decoder->vtable.on_response(code_val, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    s_set_line_state(decoder, s_linestate_header);
    return AWS_OP_SUCCESS;
}

struct aws_h1_decoder *aws_h1_decoder_new(struct aws_h1_decoder_params *params) {
    AWS_ASSERT(params);

    struct aws_h1_decoder *decoder = aws_mem_acquire(params->alloc, sizeof(struct aws_h1_decoder));
    if (!decoder) {
        return NULL;
    }
    AWS_ZERO_STRUCT(*decoder);

    decoder->alloc = params->alloc;
    decoder->user_data = params->user_data;
    decoder->vtable = params->vtable;
    decoder->is_decoding_requests = params->is_decoding_requests;

    aws_byte_buf_init(&decoder->scratch_space, params->alloc, params->scratch_space_initial_size);

    s_reset_state(decoder);

    return decoder;
}

void aws_h1_decoder_destroy(struct aws_h1_decoder *decoder) {
    if (!decoder) {
        return;
    }
    aws_byte_buf_clean_up(&decoder->scratch_space);
    aws_mem_release(decoder->alloc, decoder);
}

int aws_h1_decode(struct aws_h1_decoder *decoder, struct aws_byte_cursor *data) {
    AWS_ASSERT(decoder);
    AWS_ASSERT(data);

    struct aws_byte_cursor backup = *data;

    while (data->len && !decoder->is_done) {
        int err = decoder->run_state(decoder, data);
        if (err) {
            /* Reset the data param to how we found it */
            *data = backup;
            return AWS_OP_ERR;
        }
    }

    if (decoder->is_done) {
        s_reset_state(decoder);
    }

    return AWS_OP_SUCCESS;
}

int aws_h1_decoder_get_encoding_flags(const struct aws_h1_decoder *decoder) {
    return decoder->transfer_encoding;
}

uint64_t aws_h1_decoder_get_content_length(const struct aws_h1_decoder *decoder) {
    return decoder->content_length;
}

bool aws_h1_decoder_get_body_headers_ignored(const struct aws_h1_decoder *decoder) {
    return decoder->body_headers_ignored;
}

enum aws_http_header_block aws_h1_decoder_get_header_block(const struct aws_h1_decoder *decoder) {
    return decoder->header_block;
}

void aws_h1_decoder_set_logging_id(struct aws_h1_decoder *decoder, const void *id) {
    decoder->logging_id = id;
}

void aws_h1_decoder_set_body_headers_ignored(struct aws_h1_decoder *decoder, bool body_headers_ignored) {
    decoder->body_headers_ignored = body_headers_ignored;
}
