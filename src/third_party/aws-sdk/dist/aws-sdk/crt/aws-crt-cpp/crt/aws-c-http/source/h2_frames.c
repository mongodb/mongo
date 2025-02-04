/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/h2_frames.h>

#include <aws/compression/huffman.h>

#include <aws/common/logging.h>

#include <aws/io/stream.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

#define ENCODER_LOGF(level, encoder, text, ...)                                                                        \
    AWS_LOGF_##level(AWS_LS_HTTP_ENCODER, "id=%p " text, (encoder)->logging_id, __VA_ARGS__)

#define ENCODER_LOG(level, encoder, text) ENCODER_LOGF(level, encoder, "%s", text)

const struct aws_byte_cursor aws_h2_connection_preface_client_string =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");

/* Initial values and bounds are from RFC-7540 6.5.2 */
const uint32_t aws_h2_settings_initial[AWS_HTTP2_SETTINGS_END_RANGE] = {
    [AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE] = 4096,
    [AWS_HTTP2_SETTINGS_ENABLE_PUSH] = 1,
    [AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS] = UINT32_MAX, /* "Initially there is no limit to this value" */
    [AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE] = AWS_H2_INIT_WINDOW_SIZE,
    [AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE] = 16384,
    [AWS_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE] = UINT32_MAX, /* "The initial value of this setting is unlimited" */
};

const uint32_t aws_h2_settings_bounds[AWS_HTTP2_SETTINGS_END_RANGE][2] = {
    [AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE][0] = 0,
    [AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE][1] = UINT32_MAX,

    [AWS_HTTP2_SETTINGS_ENABLE_PUSH][0] = 0,
    [AWS_HTTP2_SETTINGS_ENABLE_PUSH][1] = 1,

    [AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS][0] = 0,
    [AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS][1] = UINT32_MAX,

    [AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE][0] = 0,
    [AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE][1] = AWS_H2_WINDOW_UPDATE_MAX,

    [AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE][0] = 16384,
    [AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE][1] = AWS_H2_PAYLOAD_MAX,

    [AWS_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE][0] = 0,
    [AWS_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE][1] = UINT32_MAX,
};

/* Stream ids & dependencies should only write the bottom 31 bits */
static const uint32_t s_u32_top_bit_mask = UINT32_MAX << 31;

/* Bytes to initially reserve for encoding of an entire header block. Buffer will grow if necessary. */
static const size_t s_encoded_header_block_reserve = 128; /* Value pulled from thin air */

#define DEFINE_FRAME_VTABLE(NAME)                                                                                      \
    static aws_h2_frame_destroy_fn s_frame_##NAME##_destroy;                                                           \
    static aws_h2_frame_encode_fn s_frame_##NAME##_encode;                                                             \
    static const struct aws_h2_frame_vtable s_frame_##NAME##_vtable = {                                                \
        .destroy = s_frame_##NAME##_destroy,                                                                           \
        .encode = s_frame_##NAME##_encode,                                                                             \
    }

const char *aws_h2_frame_type_to_str(enum aws_h2_frame_type type) {
    switch (type) {
        case AWS_H2_FRAME_T_DATA:
            return "DATA";
        case AWS_H2_FRAME_T_HEADERS:
            return "HEADERS";
        case AWS_H2_FRAME_T_PRIORITY:
            return "PRIORITY";
        case AWS_H2_FRAME_T_RST_STREAM:
            return "RST_STREAM";
        case AWS_H2_FRAME_T_SETTINGS:
            return "SETTINGS";
        case AWS_H2_FRAME_T_PUSH_PROMISE:
            return "PUSH_PROMISE";
        case AWS_H2_FRAME_T_PING:
            return "PING";
        case AWS_H2_FRAME_T_GOAWAY:
            return "GOAWAY";
        case AWS_H2_FRAME_T_WINDOW_UPDATE:
            return "WINDOW_UPDATE";
        case AWS_H2_FRAME_T_CONTINUATION:
            return "CONTINUATION";
        default:
            return "**UNKNOWN**";
    }
}

const char *aws_http2_error_code_to_str(enum aws_http2_error_code h2_error_code) {
    switch (h2_error_code) {
        case AWS_HTTP2_ERR_NO_ERROR:
            return "NO_ERROR";
        case AWS_HTTP2_ERR_PROTOCOL_ERROR:
            return "PROTOCOL_ERROR";
        case AWS_HTTP2_ERR_INTERNAL_ERROR:
            return "INTERNAL_ERROR";
        case AWS_HTTP2_ERR_FLOW_CONTROL_ERROR:
            return "FLOW_CONTROL_ERROR";
        case AWS_HTTP2_ERR_SETTINGS_TIMEOUT:
            return "SETTINGS_TIMEOUT";
        case AWS_HTTP2_ERR_STREAM_CLOSED:
            return "STREAM_CLOSED";
        case AWS_HTTP2_ERR_FRAME_SIZE_ERROR:
            return "FRAME_SIZE_ERROR";
        case AWS_HTTP2_ERR_REFUSED_STREAM:
            return "REFUSED_STREAM";
        case AWS_HTTP2_ERR_CANCEL:
            return "CANCEL";
        case AWS_HTTP2_ERR_COMPRESSION_ERROR:
            return "COMPRESSION_ERROR";
        case AWS_HTTP2_ERR_CONNECT_ERROR:
            return "CONNECT_ERROR";
        case AWS_HTTP2_ERR_ENHANCE_YOUR_CALM:
            return "ENHANCE_YOUR_CALM";
        case AWS_HTTP2_ERR_INADEQUATE_SECURITY:
            return "INADEQUATE_SECURITY";
        case AWS_HTTP2_ERR_HTTP_1_1_REQUIRED:
            return "HTTP_1_1_REQUIRED";
        default:
            return "UNKNOWN_ERROR";
    }
}

struct aws_h2err aws_h2err_from_h2_code(enum aws_http2_error_code h2_error_code) {
    AWS_PRECONDITION(h2_error_code > AWS_HTTP2_ERR_NO_ERROR && h2_error_code < AWS_HTTP2_ERR_COUNT);

    return (struct aws_h2err){
        .h2_code = h2_error_code,
        .aws_code = AWS_ERROR_HTTP_PROTOCOL_ERROR,
    };
}

struct aws_h2err aws_h2err_from_aws_code(int aws_error_code) {
    AWS_PRECONDITION(aws_error_code != 0);

    return (struct aws_h2err){
        .h2_code = AWS_HTTP2_ERR_INTERNAL_ERROR,
        .aws_code = aws_error_code,
    };
}

struct aws_h2err aws_h2err_from_last_error(void) {
    return aws_h2err_from_aws_code(aws_last_error());
}

bool aws_h2err_success(struct aws_h2err err) {
    return err.h2_code == 0 && err.aws_code == 0;
}

bool aws_h2err_failed(struct aws_h2err err) {
    return err.h2_code != 0 || err.aws_code != 0;
}

int aws_h2_validate_stream_id(uint32_t stream_id) {
    if (stream_id == 0 || stream_id > AWS_H2_STREAM_ID_MAX) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    return AWS_OP_SUCCESS;
}

/**
 * Determine max frame payload length that will:
 * 1) fit in output's available space
 * 2) obey encoders current MAX_FRAME_SIZE
 *
 * Assumes no part of the frame has been written yet to output.
 * The total length of the frame would be: returned-payload-len + AWS_H2_FRAME_PREFIX_SIZE
 *
 * Raises error if there is not enough space available for even a frame prefix.
 */
static int s_get_max_contiguous_payload_length(
    const struct aws_h2_frame_encoder *encoder,
    const struct aws_byte_buf *output,
    size_t *max_payload_length) {

    const size_t space_available = output->capacity - output->len;

    size_t max_payload_given_space_available;
    if (aws_sub_size_checked(space_available, AWS_H2_FRAME_PREFIX_SIZE, &max_payload_given_space_available)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t max_payload_given_settings = encoder->settings.max_frame_size;

    *max_payload_length = aws_min_size(max_payload_given_space_available, max_payload_given_settings);
    return AWS_OP_SUCCESS;
}

/***********************************************************************************************************************
 * Priority
 **********************************************************************************************************************/
static size_t s_frame_priority_settings_size = 5;

static void s_frame_priority_settings_encode(
    const struct aws_h2_frame_priority_settings *priority,
    struct aws_byte_buf *output) {
    AWS_PRECONDITION(priority);
    AWS_PRECONDITION(output);
    AWS_PRECONDITION((priority->stream_dependency & s_u32_top_bit_mask) == 0);
    (void)s_u32_top_bit_mask;

    /* PRIORITY is encoded as (RFC-7540 6.3):
     * +-+-------------------------------------------------------------+
     * |E|                  Stream Dependency (31)                     |
     * +-+-------------+-----------------------------------------------+
     * |   Weight (8)  |
     * +-+-------------+
     */
    bool writes_ok = true;

    /* Write the top 4 bytes */
    uint32_t top_bytes = priority->stream_dependency | ((uint32_t)priority->stream_dependency_exclusive << 31);
    writes_ok &= aws_byte_buf_write_be32(output, top_bytes);

    /* Write the priority weight */
    writes_ok &= aws_byte_buf_write_u8(output, priority->weight);

    AWS_ASSERT(writes_ok);
    (void)writes_ok;
}

/***********************************************************************************************************************
 * Common Frame Prefix
 **********************************************************************************************************************/
static void s_init_frame_base(
    struct aws_h2_frame *frame_base,
    struct aws_allocator *alloc,
    enum aws_h2_frame_type type,
    const struct aws_h2_frame_vtable *vtable,
    uint32_t stream_id) {

    frame_base->vtable = vtable;
    frame_base->alloc = alloc;
    frame_base->type = type;
    frame_base->stream_id = stream_id;
}

static void s_frame_prefix_encode(
    enum aws_h2_frame_type type,
    uint32_t stream_id,
    size_t length,
    uint8_t flags,
    struct aws_byte_buf *output) {
    AWS_PRECONDITION(output);
    AWS_PRECONDITION(!(stream_id & s_u32_top_bit_mask), "Invalid stream ID");
    AWS_PRECONDITION(length <= AWS_H2_PAYLOAD_MAX);

    /* Frame prefix is encoded like this (RFC-7540 4.1):
     * +-----------------------------------------------+
     * |                 Length (24)                   |
     * +---------------+---------------+---------------+
     * |   Type (8)    |   Flags (8)   |
     * +-+-------------+---------------+-------------------------------+
     * |R|                 Stream Identifier (31)                      |
     * +=+=============================================================+
     */
    bool writes_ok = true;

    /* Write length */
    writes_ok &= aws_byte_buf_write_be24(output, (uint32_t)length);

    /* Write type */
    writes_ok &= aws_byte_buf_write_u8(output, type);

    /* Write flags */
    writes_ok &= aws_byte_buf_write_u8(output, flags);

    /* Write stream id (with reserved first bit) */
    writes_ok &= aws_byte_buf_write_be32(output, stream_id);

    AWS_ASSERT(writes_ok);
    (void)writes_ok;
}

/***********************************************************************************************************************
 * Encoder
 **********************************************************************************************************************/
int aws_h2_frame_encoder_init(
    struct aws_h2_frame_encoder *encoder,
    struct aws_allocator *allocator,
    const void *logging_id) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(allocator);

    AWS_ZERO_STRUCT(*encoder);
    encoder->allocator = allocator;
    encoder->logging_id = logging_id;

    aws_hpack_encoder_init(&encoder->hpack, allocator, logging_id);

    encoder->settings.max_frame_size = aws_h2_settings_initial[AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE];
    return AWS_OP_SUCCESS;
}
void aws_h2_frame_encoder_clean_up(struct aws_h2_frame_encoder *encoder) {
    AWS_PRECONDITION(encoder);

    aws_hpack_encoder_clean_up(&encoder->hpack);
}

/***********************************************************************************************************************
 * DATA
 **********************************************************************************************************************/
int aws_h2_encode_data_frame(
    struct aws_h2_frame_encoder *encoder,
    uint32_t stream_id,
    struct aws_input_stream *body_stream,
    bool body_ends_stream,
    uint8_t pad_length,
    int32_t *stream_window_size_peer,
    size_t *connection_window_size_peer,
    struct aws_byte_buf *output,
    bool *body_complete,
    bool *body_stalled) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(body_stream);
    AWS_PRECONDITION(output);
    AWS_PRECONDITION(body_complete);
    AWS_PRECONDITION(body_stalled);
    AWS_PRECONDITION(*stream_window_size_peer > 0);

    if (aws_h2_validate_stream_id(stream_id)) {
        return AWS_OP_ERR;
    }

    *body_complete = false;
    *body_stalled = false;
    uint8_t flags = 0;

    /*
     * Payload-length is the first thing encoded in a frame, but we don't know how
     * much data we'll get from the body-stream until we actually read it.
     * Therefore, we determine the exact location that the body data should go,
     * then stream the body directly into that part of the output buffer.
     * Then we will go and write the other parts of the frame in around it.
     */

    size_t bytes_preceding_body = AWS_H2_FRAME_PREFIX_SIZE;
    size_t payload_overhead = 0; /* Amount of "payload" that will not contain body (padding) */
    if (pad_length > 0) {
        flags |= AWS_H2_FRAME_F_PADDED;

        /* Padding len is 1st byte of payload (padding itself goes at end of payload) */
        bytes_preceding_body += 1;
        payload_overhead = 1 + pad_length;
    }

    /* Max amount allowed by stream and connection flow-control window */
    size_t min_window_size = aws_min_size(*stream_window_size_peer, *connection_window_size_peer);

    /* Max amount of payload we can do right now */
    size_t max_payload;
    if (s_get_max_contiguous_payload_length(encoder, output, &max_payload)) {
        goto handle_waiting_for_more_space;
    }
    /* The flow-control window will limit the size for max_payload of a flow-controlled frame */
    max_payload = aws_min_size(max_payload, min_window_size);
    /* Max amount of body we can fit in the payload*/
    size_t max_body;
    if (aws_sub_size_checked(max_payload, payload_overhead, &max_body) || max_body == 0) {
        goto handle_waiting_for_more_space;
    }

    /* Use a sub-buffer to limit where body can go */
    struct aws_byte_buf body_sub_buf =
        aws_byte_buf_from_empty_array(output->buffer + output->len + bytes_preceding_body, max_body);

    /* Read body into sub-buffer */
    if (aws_input_stream_read(body_stream, &body_sub_buf)) {
        goto error;
    }

    /* Check if we've reached the end of the body */
    struct aws_stream_status body_status;
    if (aws_input_stream_get_status(body_stream, &body_status)) {
        goto error;
    }

    if (body_status.is_end_of_stream) {
        *body_complete = true;
        if (body_ends_stream) {
            flags |= AWS_H2_FRAME_F_END_STREAM;
        }
    } else {
        if (body_sub_buf.len < body_sub_buf.capacity) {
            /* Body stream was unable to provide as much data as it could have */
            *body_stalled = true;

            if (body_sub_buf.len == 0) {
                /* This frame would have no useful information, don't even bother sending it */
                goto handle_nothing_to_send_right_now;
            }
        }
    }

    ENCODER_LOGF(
        TRACE,
        encoder,
        "Encoding frame type=DATA stream_id=%" PRIu32 " data_len=%zu stalled=%d%s",
        stream_id,
        body_sub_buf.len,
        *body_stalled,
        (flags & AWS_H2_FRAME_F_END_STREAM) ? " END_STREAM" : "");

    /*
     * Write in the other parts of the frame.
     */
    bool writes_ok = true;

    /* Write the frame prefix */
    const size_t payload_len = body_sub_buf.len + payload_overhead;
    s_frame_prefix_encode(AWS_H2_FRAME_T_DATA, stream_id, payload_len, flags, output);

    /* Write pad length */
    if (flags & AWS_H2_FRAME_F_PADDED) {
        writes_ok &= aws_byte_buf_write_u8(output, pad_length);
    }

    /* Increment output->len to jump over the body that we already wrote in */
    AWS_ASSERT(output->buffer + output->len == body_sub_buf.buffer && "Streamed DATA to wrong position");
    output->len += body_sub_buf.len;

    /* Write padding */
    if (flags & AWS_H2_FRAME_F_PADDED) {
        writes_ok &= aws_byte_buf_write_u8_n(output, 0, pad_length);
    }

    /* update the connection window size now, we will update stream window size when this function returns */
    AWS_ASSERT(payload_len <= min_window_size);
    *connection_window_size_peer -= payload_len;
    *stream_window_size_peer -= (int32_t)payload_len;

    AWS_ASSERT(writes_ok);
    (void)writes_ok;
    return AWS_OP_SUCCESS;

handle_waiting_for_more_space:
    ENCODER_LOGF(TRACE, encoder, "Insufficient space to encode DATA for stream %" PRIu32 " right now", stream_id);
    return AWS_OP_SUCCESS;

handle_nothing_to_send_right_now:
    ENCODER_LOGF(INFO, encoder, "Stream %" PRIu32 " produced 0 bytes of body data", stream_id);
    return AWS_OP_SUCCESS;

error:
    return AWS_OP_ERR;
}

/***********************************************************************************************************************
 * HEADERS / PUSH_PROMISE
 **********************************************************************************************************************/
DEFINE_FRAME_VTABLE(headers);

/* Represents a HEADERS or PUSH_PROMISE frame (followed by zero or more CONTINUATION frames) */
struct aws_h2_frame_headers {
    struct aws_h2_frame base;

    /* Common data */
    const struct aws_http_headers *headers;
    uint8_t pad_length; /* Set to 0 to disable AWS_H2_FRAME_F_PADDED */

    /* HEADERS-only data */
    bool end_stream;   /* AWS_H2_FRAME_F_END_STREAM */
    bool has_priority; /* AWS_H2_FRAME_F_PRIORITY */
    struct aws_h2_frame_priority_settings priority;

    /* PUSH_PROMISE-only data */
    uint32_t promised_stream_id;

    /* State */
    enum {
        AWS_H2_HEADERS_STATE_INIT,
        AWS_H2_HEADERS_STATE_FIRST_FRAME,  /* header-block pre-encoded, no frames written yet */
        AWS_H2_HEADERS_STATE_CONTINUATION, /* first frame written, need to write CONTINUATION frames now */
        AWS_H2_HEADERS_STATE_COMPLETE,
    } state;

    struct aws_byte_buf whole_encoded_header_block;
    struct aws_byte_cursor header_block_cursor; /* tracks progress sending encoded header-block in fragments */
};

static struct aws_h2_frame *s_frame_new_headers_or_push_promise(
    struct aws_allocator *allocator,
    enum aws_h2_frame_type frame_type,
    uint32_t stream_id,
    const struct aws_http_headers *headers,
    uint8_t pad_length,
    bool end_stream,
    const struct aws_h2_frame_priority_settings *optional_priority,
    uint32_t promised_stream_id) {

    /* TODO: Host and ":authority" are no longer permitted to disagree. Should we enforce it here or sent it as
     * requested, let the server side reject the request? */
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(frame_type == AWS_H2_FRAME_T_HEADERS || frame_type == AWS_H2_FRAME_T_PUSH_PROMISE);
    AWS_PRECONDITION(headers);

    /* Validate args */

    if (aws_h2_validate_stream_id(stream_id)) {
        return NULL;
    }

    if (frame_type == AWS_H2_FRAME_T_PUSH_PROMISE) {
        if (aws_h2_validate_stream_id(promised_stream_id)) {
            return NULL;
        }
    }

    if (optional_priority && aws_h2_validate_stream_id(optional_priority->stream_dependency)) {
        return NULL;
    }

    /* Create */

    struct aws_h2_frame_headers *frame = aws_mem_calloc(allocator, 1, sizeof(struct aws_h2_frame_headers));
    if (!frame) {
        return NULL;
    }

    if (aws_byte_buf_init(&frame->whole_encoded_header_block, allocator, s_encoded_header_block_reserve)) {
        goto error;
    }

    if (frame_type == AWS_H2_FRAME_T_HEADERS) {
        frame->end_stream = end_stream;
        if (optional_priority) {
            frame->has_priority = true;
            frame->priority = *optional_priority;
        }
    } else {
        frame->promised_stream_id = promised_stream_id;
    }

    s_init_frame_base(&frame->base, allocator, frame_type, &s_frame_headers_vtable, stream_id);

    aws_http_headers_acquire((struct aws_http_headers *)headers);
    frame->headers = headers;
    frame->pad_length = pad_length;

    return &frame->base;

error:
    s_frame_headers_destroy(&frame->base);
    return NULL;
}

struct aws_h2_frame *aws_h2_frame_new_headers(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    const struct aws_http_headers *headers,
    bool end_stream,
    uint8_t pad_length,
    const struct aws_h2_frame_priority_settings *optional_priority) {

    return s_frame_new_headers_or_push_promise(
        allocator,
        AWS_H2_FRAME_T_HEADERS,
        stream_id,
        headers,
        pad_length,
        end_stream,
        optional_priority,
        0 /* HEADERS doesn't have promised_stream_id */);
}

struct aws_h2_frame *aws_h2_frame_new_push_promise(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t promised_stream_id,
    const struct aws_http_headers *headers,
    uint8_t pad_length) {

    return s_frame_new_headers_or_push_promise(
        allocator,
        AWS_H2_FRAME_T_PUSH_PROMISE,
        stream_id,
        headers,
        pad_length,
        false /* PUSH_PROMISE doesn't have end_stream flag */,
        NULL /* PUSH_PROMISE doesn't have priority_settings */,
        promised_stream_id);
}

static void s_frame_headers_destroy(struct aws_h2_frame *frame_base) {
    struct aws_h2_frame_headers *frame = AWS_CONTAINER_OF(frame_base, struct aws_h2_frame_headers, base);
    aws_http_headers_release((struct aws_http_headers *)frame->headers);
    aws_byte_buf_clean_up(&frame->whole_encoded_header_block);
    aws_mem_release(frame->base.alloc, frame);
}

/* Encode the next frame for this header-block (or encode nothing if output buffer is too small). */
static void s_encode_single_header_block_frame(
    struct aws_h2_frame_headers *frame,
    struct aws_h2_frame_encoder *encoder,
    struct aws_byte_buf *output,
    bool *waiting_for_more_space) {

    /*
     * Figure out the details of the next frame to encode.
     * The first frame will be either HEADERS or PUSH_PROMISE.
     * All subsequent frames will be CONTINUATION
     */

    enum aws_h2_frame_type frame_type;
    uint8_t flags = 0;
    uint8_t pad_length = 0;
    const struct aws_h2_frame_priority_settings *priority_settings = NULL;
    const uint32_t *promised_stream_id = NULL;
    size_t payload_overhead = 0; /* Amount of payload holding things other than header-block (padding, etc) */

    if (frame->state == AWS_H2_HEADERS_STATE_FIRST_FRAME) {
        frame_type = frame->base.type;

        if (frame->pad_length > 0) {
            flags |= AWS_H2_FRAME_F_PADDED;
            pad_length = frame->pad_length;
            payload_overhead += 1 + pad_length;
        }

        if (frame->has_priority) {
            priority_settings = &frame->priority;
            flags |= AWS_H2_FRAME_F_PRIORITY;
            payload_overhead += s_frame_priority_settings_size;
        }

        if (frame->end_stream) {
            flags |= AWS_H2_FRAME_F_END_STREAM;
        }

        if (frame_type == AWS_H2_FRAME_T_PUSH_PROMISE) {
            promised_stream_id = &frame->promised_stream_id;
            payload_overhead += 4;
        }

    } else /* CONTINUATION */ {
        frame_type = AWS_H2_FRAME_T_CONTINUATION;
    }

    /*
     * Figure out what size header-block fragment should go in this frame.
     */

    size_t max_payload;
    if (s_get_max_contiguous_payload_length(encoder, output, &max_payload)) {
        goto handle_waiting_for_more_space;
    }

    size_t max_fragment;
    if (aws_sub_size_checked(max_payload, payload_overhead, &max_fragment)) {
        goto handle_waiting_for_more_space;
    }

    const size_t fragment_len = aws_min_size(max_fragment, frame->header_block_cursor.len);
    if (fragment_len == frame->header_block_cursor.len) {
        /* This will finish the header-block */
        flags |= AWS_H2_FRAME_F_END_HEADERS;
    } else {
        /* If we're not finishing the header-block, is it even worth trying to send this frame now? */
        const size_t even_worth_sending_threshold = AWS_H2_FRAME_PREFIX_SIZE + payload_overhead;
        if (fragment_len < even_worth_sending_threshold) {
            goto handle_waiting_for_more_space;
        }
    }

    /*
     * Ok, it fits! Write the frame
     */
    ENCODER_LOGF(
        TRACE,
        encoder,
        "Encoding frame type=%s stream_id=%" PRIu32 "%s%s",
        aws_h2_frame_type_to_str(frame_type),
        frame->base.stream_id,
        (flags & AWS_H2_FRAME_F_END_HEADERS) ? " END_HEADERS" : "",
        (flags & AWS_H2_FRAME_F_END_STREAM) ? " END_STREAM" : "");

    bool writes_ok = true;

    /* Write the frame prefix */
    const size_t payload_len = fragment_len + payload_overhead;
    s_frame_prefix_encode(frame_type, frame->base.stream_id, payload_len, flags, output);

    /* Write pad length */
    if (flags & AWS_H2_FRAME_F_PADDED) {
        AWS_ASSERT(frame_type != AWS_H2_FRAME_T_CONTINUATION);
        writes_ok &= aws_byte_buf_write_u8(output, pad_length);
    }

    /* Write priority */
    if (flags & AWS_H2_FRAME_F_PRIORITY) {
        AWS_ASSERT(frame_type == AWS_H2_FRAME_T_HEADERS);
        s_frame_priority_settings_encode(priority_settings, output);
    }

    /* Write promised stream ID */
    if (promised_stream_id) {
        AWS_ASSERT(frame_type == AWS_H2_FRAME_T_PUSH_PROMISE);
        writes_ok &= aws_byte_buf_write_be32(output, *promised_stream_id);
    }

    /* Write header-block fragment */
    if (fragment_len > 0) {
        struct aws_byte_cursor fragment = aws_byte_cursor_advance(&frame->header_block_cursor, fragment_len);
        writes_ok &= aws_byte_buf_write_from_whole_cursor(output, fragment);
    }

    /* Write padding */
    if (flags & AWS_H2_FRAME_F_PADDED) {
        writes_ok &= aws_byte_buf_write_u8_n(output, 0, pad_length);
    }

    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    /* Success! Wrote entire frame. It's safe to change state now */
    frame->state =
        flags & AWS_H2_FRAME_F_END_HEADERS ? AWS_H2_HEADERS_STATE_COMPLETE : AWS_H2_HEADERS_STATE_CONTINUATION;
    *waiting_for_more_space = false;
    return;

handle_waiting_for_more_space:
    ENCODER_LOGF(
        TRACE,
        encoder,
        "Insufficient space to encode %s for stream %" PRIu32 " right now",
        aws_h2_frame_type_to_str(frame->base.type),
        frame->base.stream_id);
    *waiting_for_more_space = true;
}

static int s_frame_headers_encode(
    struct aws_h2_frame *frame_base,
    struct aws_h2_frame_encoder *encoder,
    struct aws_byte_buf *output,
    bool *complete) {

    struct aws_h2_frame_headers *frame = AWS_CONTAINER_OF(frame_base, struct aws_h2_frame_headers, base);

    /* Pre-encode the entire header-block into another buffer
     * the first time we're called. */
    if (frame->state == AWS_H2_HEADERS_STATE_INIT) {
        if (aws_hpack_encode_header_block(&encoder->hpack, frame->headers, &frame->whole_encoded_header_block)) {
            ENCODER_LOGF(
                ERROR,
                encoder,
                "Error doing HPACK encoding on %s of stream %" PRIu32 ": %s",
                aws_h2_frame_type_to_str(frame->base.type),
                frame->base.stream_id,
                aws_error_name(aws_last_error()));
            goto error;
        }

        frame->header_block_cursor = aws_byte_cursor_from_buf(&frame->whole_encoded_header_block);
        frame->state = AWS_H2_HEADERS_STATE_FIRST_FRAME;
    }

    /* Write frames (HEADER or PUSH_PROMISE, followed by N CONTINUATION frames)
     * until we're done writing header-block or the buffer is too full to continue */
    bool waiting_for_more_space = false;
    while (frame->state < AWS_H2_HEADERS_STATE_COMPLETE && !waiting_for_more_space) {
        s_encode_single_header_block_frame(frame, encoder, output, &waiting_for_more_space);
    }

    *complete = frame->state == AWS_H2_HEADERS_STATE_COMPLETE;
    return AWS_OP_SUCCESS;

error:
    return AWS_OP_ERR;
}

/***********************************************************************************************************************
 * aws_h2_frame_prebuilt - Used by small simple frame types that we can pre-encode at the time of creation.
 * The pre-encoded buffer is then just copied bit-by-bit during the actual "encode()" function.
 *
 * It's safe to pre-encode a frame if it doesn't query/mutate any external state. So PING is totally great
 * to pre-encode, but HEADERS (which queries MAX_FRAME_SIZE and mutates the HPACK table) would be a bad candidate.
 **********************************************************************************************************************/
struct aws_h2_frame_prebuilt {
    struct aws_h2_frame base;

    /* The whole entire frame is pre-encoded to this buffer during construction.
     * The buffer has the exact capacity necessary to hold the frame */
    struct aws_byte_buf encoded_buf;

    /* After construction, this cursor points to the full contents of encoded_buf.
     * As encode() is called, we copy the contents to output and advance the cursor.*/
    struct aws_byte_cursor cursor;
};

DEFINE_FRAME_VTABLE(prebuilt);

/* Can't pre-encode a frame unless it's guaranteed to fit, regardless of current settings. */
static size_t s_prebuilt_payload_max(void) {
    return aws_h2_settings_bounds[AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE][0];
}

/* Create aws_h2_frame_prebuilt and encode frame prefix into frame->encoded_buf.
 * Caller must encode the payload to fill the rest of the encoded_buf. */
static struct aws_h2_frame_prebuilt *s_h2_frame_new_prebuilt(
    struct aws_allocator *allocator,
    enum aws_h2_frame_type type,
    uint32_t stream_id,
    size_t payload_len,
    uint8_t flags) {

    AWS_PRECONDITION(payload_len <= s_prebuilt_payload_max());

    const size_t encoded_frame_len = AWS_H2_FRAME_PREFIX_SIZE + payload_len;

    /* Use single allocation for frame and buffer storage */
    struct aws_h2_frame_prebuilt *frame;
    void *storage;
    if (!aws_mem_acquire_many(
            allocator, 2, &frame, sizeof(struct aws_h2_frame_prebuilt), &storage, encoded_frame_len)) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*frame);
    s_init_frame_base(&frame->base, allocator, type, &s_frame_prebuilt_vtable, stream_id);

    /* encoded_buf has the exact amount of space necessary for the full encoded frame.
     * The constructor of our subclass must finish filling up encoded_buf with the payload. */
    frame->encoded_buf = aws_byte_buf_from_empty_array(storage, encoded_frame_len);

    /* cursor points to full capacity of encoded_buf.
     * Our subclass's constructor will finish writing the payload and fill encoded_buf to capacity.
     * When encode() is called, we'll copy cursor's contents into available output space and advance the cursor. */
    frame->cursor = aws_byte_cursor_from_array(storage, encoded_frame_len);

    /* Write frame prefix */
    s_frame_prefix_encode(type, stream_id, payload_len, flags, &frame->encoded_buf);

    return frame;
}

static void s_frame_prebuilt_destroy(struct aws_h2_frame *frame_base) {
    aws_mem_release(frame_base->alloc, frame_base);
}

static int s_frame_prebuilt_encode(
    struct aws_h2_frame *frame_base,
    struct aws_h2_frame_encoder *encoder,
    struct aws_byte_buf *output,
    bool *complete) {

    (void)encoder;
    struct aws_h2_frame_prebuilt *frame = AWS_CONTAINER_OF(frame_base, struct aws_h2_frame_prebuilt, base);

    /* encoded_buf should have been filled to capacity during construction */
    AWS_ASSERT(frame->encoded_buf.len == frame->encoded_buf.capacity);

    /* After construction, cursor points to the full contents of encoded_buf.
     * As encode() is called, we copy the contents to output and advance the cursor. */
    if (frame->cursor.len == frame->encoded_buf.len) {
        /* We haven't sent anything yet, announce start of frame */
        ENCODER_LOGF(
            TRACE,
            encoder,
            "Encoding frame type=%s stream_id=%" PRIu32,
            aws_h2_frame_type_to_str(frame->base.type),
            frame->base.stream_id);
    } else {
        /* We've already sent a bit, announce that we're resuming */
        ENCODER_LOGF(
            TRACE,
            encoder,
            "Resume encoding frame type=%s stream_id=%" PRIu32,
            aws_h2_frame_type_to_str(frame->base.type),
            frame->base.stream_id);
    }

    bool writes_ok = true;

    /* Copy as much as we can from cursor (pre-encoded frame contents) to output.
     * Advance the cursor to mark our progress. */
    size_t chunk_len = aws_min_size(frame->cursor.len, output->capacity - output->len);
    struct aws_byte_cursor chunk = aws_byte_cursor_advance(&frame->cursor, chunk_len);
    writes_ok &= aws_byte_buf_write_from_whole_cursor(output, chunk);
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    if (frame->cursor.len == 0) {
        *complete = true;
    } else {
        ENCODER_LOGF(
            TRACE,
            encoder,
            "Incomplete encoding of frame type=%s stream_id=%" PRIu32 ", will resume later...",
            aws_h2_frame_type_to_str(frame->base.type),
            frame->base.stream_id);

        *complete = false;
    }
    return AWS_OP_SUCCESS;
}

/***********************************************************************************************************************
 * PRIORITY
 **********************************************************************************************************************/
struct aws_h2_frame *aws_h2_frame_new_priority(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    const struct aws_h2_frame_priority_settings *priority) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(priority);

    if (aws_h2_validate_stream_id(stream_id) || aws_h2_validate_stream_id(priority->stream_dependency)) {
        return NULL;
    }

    /* PRIORITY can be pre-encoded */
    const uint8_t flags = 0;
    const size_t payload_len = s_frame_priority_settings_size;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_PRIORITY, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write the priority settings */
    s_frame_priority_settings_encode(priority, &frame->encoded_buf);

    return &frame->base;
}

/***********************************************************************************************************************
 * RST_STREAM
 **********************************************************************************************************************/
static const size_t s_frame_rst_stream_length = 4;

struct aws_h2_frame *aws_h2_frame_new_rst_stream(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t error_code) {

    if (aws_h2_validate_stream_id(stream_id)) {
        return NULL;
    }

    /* RST_STREAM can be pre-encoded */
    const uint8_t flags = 0;
    const size_t payload_len = s_frame_rst_stream_length;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_RST_STREAM, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write RST_STREAM payload (RFC-7540 6.4):
     * +---------------------------------------------------------------+
     * |                        Error Code (32)                        |
     * +---------------------------------------------------------------+
     */
    bool writes_ok = true;
    writes_ok &= aws_byte_buf_write_be32(&frame->encoded_buf, error_code);
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    return &frame->base;
}

/***********************************************************************************************************************
 * SETTINGS
 **********************************************************************************************************************/
static const size_t s_frame_setting_length = 6;

struct aws_h2_frame *aws_h2_frame_new_settings(
    struct aws_allocator *allocator,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    bool ack) {

    AWS_PRECONDITION(settings_array || num_settings == 0);

    /* Cannot send settings in an ACK frame */
    if (ack && num_settings > 0) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    /* Check against insane edge case of too many settings to fit in a frame. */
    const size_t max_settings = s_prebuilt_payload_max() / s_frame_setting_length;
    if (num_settings > max_settings) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_ENCODER,
            "Cannot create SETTINGS frame with %zu settings, the limit is %zu.",
            num_settings,
            max_settings);

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    /* SETTINGS can be pre-encoded */
    const uint8_t flags = ack ? AWS_H2_FRAME_F_ACK : 0;
    const size_t payload_len = num_settings * s_frame_setting_length;
    const uint32_t stream_id = 0;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_SETTINGS, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write the settings, each one is encoded like (RFC-7540 6.5.1):
     * +-------------------------------+
     * |       Identifier (16)         |
     * +-------------------------------+-------------------------------+
     * |                        Value (32)                             |
     * +---------------------------------------------------------------+
     */
    bool writes_ok = true;
    for (size_t i = 0; i < num_settings; ++i) {
        writes_ok &= aws_byte_buf_write_be16(&frame->encoded_buf, settings_array[i].id);
        writes_ok &= aws_byte_buf_write_be32(&frame->encoded_buf, settings_array[i].value);
    }
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    return &frame->base;
}

/***********************************************************************************************************************
 * PING
 **********************************************************************************************************************/
struct aws_h2_frame *aws_h2_frame_new_ping(
    struct aws_allocator *allocator,
    bool ack,
    const uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE]) {

    /* PING can be pre-encoded */
    const uint8_t flags = ack ? AWS_H2_FRAME_F_ACK : 0;
    const size_t payload_len = AWS_HTTP2_PING_DATA_SIZE;
    const uint32_t stream_id = 0;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_PING, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write the PING payload (RFC-7540 6.7):
     * +---------------------------------------------------------------+
     * |                                                               |
     * |                      Opaque Data (64)                         |
     * |                                                               |
     * +---------------------------------------------------------------+
     */
    bool writes_ok = true;
    writes_ok &= aws_byte_buf_write(&frame->encoded_buf, opaque_data, AWS_HTTP2_PING_DATA_SIZE);
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    /* PING responses SHOULD be given higher priority than any other frame */
    frame->base.high_priority = ack;
    return &frame->base;
}

/***********************************************************************************************************************
 * GOAWAY
 **********************************************************************************************************************/
static const size_t s_frame_goaway_length_min = 8;

struct aws_h2_frame *aws_h2_frame_new_goaway(
    struct aws_allocator *allocator,
    uint32_t last_stream_id,
    uint32_t error_code,
    struct aws_byte_cursor debug_data) {

    /* If debug_data is too long, don't sent it.
     * It's more important that the GOAWAY frame gets sent. */
    const size_t debug_data_max = s_prebuilt_payload_max() - s_frame_goaway_length_min;
    if (debug_data.len > debug_data_max) {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_ENCODER,
            "Sending GOAWAY without debug-data. Debug-data size %zu exceeds internal limit of %zu",
            debug_data.len,
            debug_data_max);

        debug_data.len = 0;
    }

    /* It would be illegal to send a lower value, this is unrecoverable */
    AWS_FATAL_ASSERT(last_stream_id <= AWS_H2_STREAM_ID_MAX);

    /* GOAWAY can be pre-encoded */
    const uint8_t flags = 0;
    const size_t payload_len = debug_data.len + s_frame_goaway_length_min;
    const uint32_t stream_id = 0;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_GOAWAY, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write the GOAWAY payload (RFC-7540 6.8):
     * +-+-------------------------------------------------------------+
     * |R|                  Last-Stream-ID (31)                        |
     * +-+-------------------------------------------------------------+
     * |                      Error Code (32)                          |
     * +---------------------------------------------------------------+
     * |                  Additional Debug Data (*)                    |
     * +---------------------------------------------------------------+
     */
    bool writes_ok = true;
    writes_ok &= aws_byte_buf_write_be32(&frame->encoded_buf, last_stream_id);
    writes_ok &= aws_byte_buf_write_be32(&frame->encoded_buf, error_code);
    writes_ok &= aws_byte_buf_write_from_whole_cursor(&frame->encoded_buf, debug_data);
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    return &frame->base;
}

/***********************************************************************************************************************
 * WINDOW_UPDATE
 **********************************************************************************************************************/
static const size_t s_frame_window_update_length = 4;

struct aws_h2_frame *aws_h2_frame_new_window_update(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t window_size_increment) {

    /* Note: stream_id may be zero or non-zero */
    if (stream_id > AWS_H2_STREAM_ID_MAX) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (window_size_increment > AWS_H2_WINDOW_UPDATE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_ENCODER,
            "Window increment size %" PRIu32 " exceeds HTTP/2 max %" PRIu32,
            window_size_increment,
            AWS_H2_WINDOW_UPDATE_MAX);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    /* WINDOW_UPDATE can be pre-encoded */
    const uint8_t flags = 0;
    const size_t payload_len = s_frame_window_update_length;

    struct aws_h2_frame_prebuilt *frame =
        s_h2_frame_new_prebuilt(allocator, AWS_H2_FRAME_T_WINDOW_UPDATE, stream_id, payload_len, flags);
    if (!frame) {
        return NULL;
    }

    /* Write the WINDOW_UPDATE payload (RFC-7540 6.9):
     * +-+-------------------------------------------------------------+
     * |R|              Window Size Increment (31)                     |
     * +-+-------------------------------------------------------------+
     */
    bool writes_ok = true;
    writes_ok &= aws_byte_buf_write_be32(&frame->encoded_buf, window_size_increment);
    AWS_ASSERT(writes_ok);
    (void)writes_ok;

    return &frame->base;
}

void aws_h2_frame_destroy(struct aws_h2_frame *frame) {
    if (frame) {
        frame->vtable->destroy(frame);
    }
}

int aws_h2_encode_frame(
    struct aws_h2_frame_encoder *encoder,
    struct aws_h2_frame *frame,
    struct aws_byte_buf *output,
    bool *frame_complete) {

    AWS_PRECONDITION(encoder);
    AWS_PRECONDITION(frame);
    AWS_PRECONDITION(output);
    AWS_PRECONDITION(frame_complete);

    if (encoder->has_errored) {
        ENCODER_LOG(ERROR, encoder, "Encoder cannot be used again after an error");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    if (encoder->current_frame && (encoder->current_frame != frame)) {
        ENCODER_LOG(ERROR, encoder, "Cannot encode new frame until previous frame completes");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    *frame_complete = false;

    if (frame->vtable->encode(frame, encoder, output, frame_complete)) {
        ENCODER_LOGF(
            ERROR,
            encoder,
            "Failed to encode frame type=%s stream_id=%" PRIu32 ", %s",
            aws_h2_frame_type_to_str(frame->type),
            frame->stream_id,
            aws_error_name(aws_last_error()));
        encoder->has_errored = true;
        return AWS_OP_ERR;
    }

    encoder->current_frame = *frame_complete ? NULL : frame;
    return AWS_OP_SUCCESS;
}

void aws_h2_frame_encoder_set_setting_header_table_size(struct aws_h2_frame_encoder *encoder, uint32_t data) {
    /* Setting for dynamic table size changed from peer, we will update the dynamic table size when we encoder the next
     * header block */
    aws_hpack_encoder_update_max_table_size(&encoder->hpack, data);
}

void aws_h2_frame_encoder_set_setting_max_frame_size(struct aws_h2_frame_encoder *encoder, uint32_t data) {
    encoder->settings.max_frame_size = data;
}
