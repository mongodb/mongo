/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/h2_decoder.h>

#include <aws/http/private/hpack.h>
#include <aws/http/private/strutil.h>

#include <aws/common/string.h>
#include <aws/http/status_code.h>
#include <aws/io/logging.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* Declared initializers */
#endif

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/

/* The scratch buffers data for states with bytes_required > 0. Must be big enough for largest state */
static const size_t s_scratch_space_size = 9;

/* Stream ids & dependencies should only write the bottom 31 bits */
static const uint32_t s_31_bit_mask = UINT32_MAX >> 1;

/* initial size for cookie buffer, buffer will grow if needed */
static const size_t s_decoder_cookie_buffer_initial_size = 512;

#define DECODER_LOGF(level, decoder, text, ...)                                                                        \
    AWS_LOGF_##level(AWS_LS_HTTP_DECODER, "id=%p " text, (decoder)->logging_id, __VA_ARGS__)
#define DECODER_LOG(level, decoder, text) DECODER_LOGF(level, decoder, "%s", text)

#define DECODER_CALL_VTABLE(decoder, fn)                                                                               \
    do {                                                                                                               \
        if ((decoder)->vtable->fn) {                                                                                   \
            DECODER_LOG(TRACE, decoder, "Invoking callback " #fn);                                                     \
            struct aws_h2err vtable_err = (decoder)->vtable->fn((decoder)->userdata);                                  \
            if (aws_h2err_failed(vtable_err)) {                                                                        \
                DECODER_LOGF(                                                                                          \
                    ERROR,                                                                                             \
                    decoder,                                                                                           \
                    "Error from callback " #fn ", %s->%s",                                                             \
                    aws_http2_error_code_to_str(vtable_err.h2_code),                                                   \
                    aws_error_name(vtable_err.aws_code));                                                              \
                return vtable_err;                                                                                     \
            }                                                                                                          \
        }                                                                                                              \
    } while (false)
#define DECODER_CALL_VTABLE_ARGS(decoder, fn, ...)                                                                     \
    do {                                                                                                               \
        if ((decoder)->vtable->fn) {                                                                                   \
            DECODER_LOG(TRACE, decoder, "Invoking callback " #fn);                                                     \
            struct aws_h2err vtable_err = (decoder)->vtable->fn(__VA_ARGS__, (decoder)->userdata);                     \
            if (aws_h2err_failed(vtable_err)) {                                                                        \
                DECODER_LOGF(                                                                                          \
                    ERROR,                                                                                             \
                    decoder,                                                                                           \
                    "Error from callback " #fn ", %s->%s",                                                             \
                    aws_http2_error_code_to_str(vtable_err.h2_code),                                                   \
                    aws_error_name(vtable_err.aws_code));                                                              \
                return vtable_err;                                                                                     \
            }                                                                                                          \
        }                                                                                                              \
    } while (false)
#define DECODER_CALL_VTABLE_STREAM(decoder, fn)                                                                        \
    DECODER_CALL_VTABLE_ARGS(decoder, fn, (decoder)->frame_in_progress.stream_id)
#define DECODER_CALL_VTABLE_STREAM_ARGS(decoder, fn, ...)                                                              \
    DECODER_CALL_VTABLE_ARGS(decoder, fn, (decoder)->frame_in_progress.stream_id, __VA_ARGS__)

/* for storing things in array without worrying about the specific values of the other AWS_HTTP_HEADER_XYZ enums */
enum pseudoheader_name {
    PSEUDOHEADER_UNKNOWN = -1, /* Unrecognized value */

    /* Request pseudo-headers */
    PSEUDOHEADER_METHOD,
    PSEUDOHEADER_SCHEME,
    PSEUDOHEADER_AUTHORITY,
    PSEUDOHEADER_PATH,
    /* Response pseudo-headers */
    PSEUDOHEADER_STATUS,

    PSEUDOHEADER_COUNT, /* Number of valid enums */
};

static const struct aws_byte_cursor *s_pseudoheader_name_to_cursor[PSEUDOHEADER_COUNT] = {
    [PSEUDOHEADER_METHOD] = &aws_http_header_method,
    [PSEUDOHEADER_SCHEME] = &aws_http_header_scheme,
    [PSEUDOHEADER_AUTHORITY] = &aws_http_header_authority,
    [PSEUDOHEADER_PATH] = &aws_http_header_path,
    [PSEUDOHEADER_STATUS] = &aws_http_header_status,
};

static const enum aws_http_header_name s_pseudoheader_to_header_name[PSEUDOHEADER_COUNT] = {
    [PSEUDOHEADER_METHOD] = AWS_HTTP_HEADER_METHOD,
    [PSEUDOHEADER_SCHEME] = AWS_HTTP_HEADER_SCHEME,
    [PSEUDOHEADER_AUTHORITY] = AWS_HTTP_HEADER_AUTHORITY,
    [PSEUDOHEADER_PATH] = AWS_HTTP_HEADER_PATH,
    [PSEUDOHEADER_STATUS] = AWS_HTTP_HEADER_STATUS,
};

static enum pseudoheader_name s_header_to_pseudoheader_name(enum aws_http_header_name name) {
    /* The compiled switch statement is actually faster than array lookup with bounds-checking.
     * (the lookup arrays above don't need to do bounds-checking) */
    switch (name) {
        case AWS_HTTP_HEADER_METHOD:
            return PSEUDOHEADER_METHOD;
        case AWS_HTTP_HEADER_SCHEME:
            return PSEUDOHEADER_SCHEME;
        case AWS_HTTP_HEADER_AUTHORITY:
            return PSEUDOHEADER_AUTHORITY;
        case AWS_HTTP_HEADER_PATH:
            return PSEUDOHEADER_PATH;
        case AWS_HTTP_HEADER_STATUS:
            return PSEUDOHEADER_STATUS;
        default:
            return PSEUDOHEADER_UNKNOWN;
    }
}

/***********************************************************************************************************************
 * State Machine
 **********************************************************************************************************************/

typedef struct aws_h2err(state_fn)(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input);
struct h2_decoder_state {
    state_fn *fn;
    uint32_t bytes_required;
    const char *name;
};

#define DEFINE_STATE(_name, _bytes_required)                                                                           \
    static state_fn s_state_fn_##_name;                                                                                \
    enum { s_state_##_name##_requires_##_bytes_required##_bytes = _bytes_required };                                   \
    static const struct h2_decoder_state s_state_##_name = {                                                           \
        .fn = s_state_fn_##_name,                                                                                      \
        .bytes_required = s_state_##_name##_requires_##_bytes_required##_bytes,                                        \
        .name = #_name,                                                                                                \
    }

/* Common states */
DEFINE_STATE(prefix, 9);
DEFINE_STATE(padding_len, 1);
DEFINE_STATE(padding, 0);

DEFINE_STATE(priority_block, 5);

DEFINE_STATE(header_block_loop, 0);
DEFINE_STATE(header_block_entry, 1); /* requires 1 byte, but may consume more */

/* Frame-specific states */
DEFINE_STATE(frame_data, 0);
DEFINE_STATE(frame_headers, 0);
DEFINE_STATE(frame_priority, 0);
DEFINE_STATE(frame_rst_stream, 4);
DEFINE_STATE(frame_settings_begin, 0);
DEFINE_STATE(frame_settings_loop, 0);
DEFINE_STATE(frame_settings_i, 6);
DEFINE_STATE(frame_push_promise, 4);
DEFINE_STATE(frame_ping, 8);
DEFINE_STATE(frame_goaway, 8);
DEFINE_STATE(frame_goaway_debug_data, 0);
DEFINE_STATE(frame_window_update, 4);
DEFINE_STATE(frame_continuation, 0);
DEFINE_STATE(frame_unknown, 0);

/* States that have nothing to do with frames */
DEFINE_STATE(connection_preface_string, 1); /* requires 1 byte but may consume more */

/* Helper for states that need to transition to frame-type states */
static const struct h2_decoder_state *s_state_frames[AWS_H2_FRAME_TYPE_COUNT] = {
    [AWS_H2_FRAME_T_DATA] = &s_state_frame_data,
    [AWS_H2_FRAME_T_HEADERS] = &s_state_frame_headers,
    [AWS_H2_FRAME_T_PRIORITY] = &s_state_frame_priority,
    [AWS_H2_FRAME_T_RST_STREAM] = &s_state_frame_rst_stream,
    [AWS_H2_FRAME_T_SETTINGS] = &s_state_frame_settings_begin,
    [AWS_H2_FRAME_T_PUSH_PROMISE] = &s_state_frame_push_promise,
    [AWS_H2_FRAME_T_PING] = &s_state_frame_ping,
    [AWS_H2_FRAME_T_GOAWAY] = &s_state_frame_goaway,
    [AWS_H2_FRAME_T_WINDOW_UPDATE] = &s_state_frame_window_update,
    [AWS_H2_FRAME_T_CONTINUATION] = &s_state_frame_continuation,
    [AWS_H2_FRAME_T_UNKNOWN] = &s_state_frame_unknown,
};

/***********************************************************************************************************************
 * Struct
 **********************************************************************************************************************/

struct aws_h2_decoder {
    /* Implementation data. */
    struct aws_allocator *alloc;
    const void *logging_id;
    struct aws_hpack_decoder hpack;
    bool is_server;
    struct aws_byte_buf scratch;
    const struct h2_decoder_state *state;
    bool state_changed;

    /* HTTP/2 connection preface must be first thing received (RFC-7540 3.5):
     * Server must receive (client must send): magic string, then SETTINGS frame.
     * Client must receive (server must send): SETTINGS frame. */
    bool connection_preface_complete;

    /* Cursor over the canonical client connection preface string */
    struct aws_byte_cursor connection_preface_cursor;

    /* Frame-in-progress */
    struct aws_frame_in_progress {
        enum aws_h2_frame_type type;
        uint32_t stream_id;
        uint32_t payload_len;
        uint8_t padding_len;

        struct {
            bool ack;
            bool end_stream;
            bool end_headers;
            bool priority;
        } flags;
    } frame_in_progress;

    /* GOAWAY buffer */
    struct aws_goaway_in_progress {
        uint32_t last_stream;
        uint32_t error_code;
        /* Buffer of the received debug data in the latest goaway frame */
        struct aws_byte_buf debug_data;
    } goaway_in_progress;

    /* A header-block starts with a HEADERS or PUSH_PROMISE frame, followed by 0 or more CONTINUATION frames.
     * It's an error for any other frame-type or stream ID to arrive while a header-block is in progress.
     * The header-block ends when a frame has the END_HEADERS flag set. (RFC-7540 4.3) */
    struct aws_header_block_in_progress {
        /* If 0, then no header-block in progress */
        uint32_t stream_id;

        /* Whether these are informational (1xx), normal, or trailing headers */
        enum aws_http_header_block block_type;

        /* Buffer up pseudo-headers and deliver them once they're all validated */
        struct aws_string *pseudoheader_values[PSEUDOHEADER_COUNT];
        enum aws_http_header_compression pseudoheader_compression[PSEUDOHEADER_COUNT];

        /* All pseudo-header fields MUST appear in the header block before regular header fields. */
        bool pseudoheaders_done;

        /* T: PUSH_PROMISE header-block
         * F: HEADERS header-block */
        bool is_push_promise;

        /* If frame that starts header-block has END_STREAM flag,
         * then frame that ends header-block also ends the stream. */
        bool ends_stream;

        /* True if something occurs that makes the header-block malformed (ex: invalid header name).
         * A malformed header-block is not a connection error, it's a Stream Error (RFC-7540 5.4.2).
         * We continue decoding and report that it's malformed in on_headers_end(). */
        bool malformed;

        bool body_headers_forbidden;

        /* Buffer up cookie header fields to concatenate separate ones */
        struct aws_byte_buf cookies;
        /* If separate cookie fields have different compression types, the concatenated cookie uses the strictest type.
         */
        enum aws_http_header_compression cookie_header_compression_type;
    } header_block_in_progress;

    /* Settings for decoder, which is based on the settings sent to the peer and ACKed by peer */
    struct {
        /* enable/disable server push */
        uint32_t enable_push;
        /*  the size of the largest frame payload */
        uint32_t max_frame_size;
    } settings;

    struct aws_array_list settings_buffer_list;

    /* User callbacks and settings. */
    const struct aws_h2_decoder_vtable *vtable;
    void *userdata;

    /* If this is set to true, decode may no longer be called */
    bool has_errored;
};

/***********************************************************************************************************************/

struct aws_h2_decoder *aws_h2_decoder_new(struct aws_h2_decoder_params *params) {
    AWS_PRECONDITION(params);
    AWS_PRECONDITION(params->alloc);
    AWS_PRECONDITION(params->vtable);

    struct aws_h2_decoder *decoder = NULL;
    void *scratch_buf = NULL;

    void *allocation = aws_mem_acquire_many(
        params->alloc, 2, &decoder, sizeof(struct aws_h2_decoder), &scratch_buf, s_scratch_space_size);
    if (!allocation) {
        goto error;
    }

    AWS_ZERO_STRUCT(*decoder);
    decoder->alloc = params->alloc;
    decoder->vtable = params->vtable;
    decoder->userdata = params->userdata;
    decoder->logging_id = params->logging_id;
    decoder->is_server = params->is_server;
    decoder->connection_preface_complete = params->skip_connection_preface;

    decoder->scratch = aws_byte_buf_from_empty_array(scratch_buf, s_scratch_space_size);

    aws_hpack_decoder_init(&decoder->hpack, params->alloc, decoder);

    if (decoder->is_server && !params->skip_connection_preface) {
        decoder->state = &s_state_connection_preface_string;
        decoder->connection_preface_cursor = aws_h2_connection_preface_client_string;
    } else {
        decoder->state = &s_state_prefix;
    }

    decoder->settings.enable_push = aws_h2_settings_initial[AWS_HTTP2_SETTINGS_ENABLE_PUSH];
    decoder->settings.max_frame_size = aws_h2_settings_initial[AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE];

    if (aws_array_list_init_dynamic(
            &decoder->settings_buffer_list, decoder->alloc, 0, sizeof(struct aws_http2_setting))) {
        goto error;
    }

    if (aws_byte_buf_init(
            &decoder->header_block_in_progress.cookies, decoder->alloc, s_decoder_cookie_buffer_initial_size)) {
        goto error;
    }

    return decoder;

error:
    if (decoder) {
        aws_hpack_decoder_clean_up(&decoder->hpack);
        aws_array_list_clean_up(&decoder->settings_buffer_list);
        aws_byte_buf_clean_up(&decoder->header_block_in_progress.cookies);
    }
    aws_mem_release(params->alloc, allocation);
    return NULL;
}

static void s_reset_header_block_in_progress(struct aws_h2_decoder *decoder) {
    for (size_t i = 0; i < PSEUDOHEADER_COUNT; ++i) {
        aws_string_destroy(decoder->header_block_in_progress.pseudoheader_values[i]);
    }
    struct aws_byte_buf cookie_backup = decoder->header_block_in_progress.cookies;
    AWS_ZERO_STRUCT(decoder->header_block_in_progress);
    decoder->header_block_in_progress.cookies = cookie_backup;
    aws_byte_buf_reset(&decoder->header_block_in_progress.cookies, false);
}

void aws_h2_decoder_destroy(struct aws_h2_decoder *decoder) {
    if (!decoder) {
        return;
    }
    aws_array_list_clean_up(&decoder->settings_buffer_list);
    aws_hpack_decoder_clean_up(&decoder->hpack);
    s_reset_header_block_in_progress(decoder);
    aws_byte_buf_clean_up(&decoder->header_block_in_progress.cookies);
    aws_byte_buf_clean_up(&decoder->goaway_in_progress.debug_data);
    aws_mem_release(decoder->alloc, decoder);
}

struct aws_h2err aws_h2_decode(struct aws_h2_decoder *decoder, struct aws_byte_cursor *data) {
    AWS_PRECONDITION(decoder);
    AWS_PRECONDITION(data);

    AWS_FATAL_ASSERT(!decoder->has_errored);

    struct aws_h2err err = AWS_H2ERR_SUCCESS;

    /* Run decoder state machine until we're no longer changing states.
     * We don't simply loop `while(data->len)` because some states consume no data,
     * and these states should run even when there is no data left. */
    do {
        decoder->state_changed = false;

        const uint32_t bytes_required = decoder->state->bytes_required;
        AWS_ASSERT(bytes_required <= decoder->scratch.capacity);
        const char *current_state_name = decoder->state->name;
        const size_t prev_data_len = data->len;
        (void)prev_data_len;

        if (!decoder->scratch.len && data->len >= bytes_required) {
            /* Easy case, there is no scratch and we have enough data, so just send it to the state */

            DECODER_LOGF(TRACE, decoder, "Running state '%s' with %zu bytes available", current_state_name, data->len);

            err = decoder->state->fn(decoder, data);
            if (aws_h2err_failed(err)) {
                goto handle_error;
            }

            AWS_ASSERT(prev_data_len - data->len >= bytes_required && "Decoder state requested more data than it used");
        } else {
            /* Otherwise, state requires a minimum amount of data and we have to use the scratch */
            size_t bytes_to_read = bytes_required - decoder->scratch.len;
            bool will_finish_state = true;

            if (bytes_to_read > data->len) {
                /* Not enough in this cursor, need to read as much as possible and then come back */
                bytes_to_read = data->len;
                will_finish_state = false;
            }

            if (AWS_LIKELY(bytes_to_read)) {
                /* Read the appropriate number of bytes into scratch */
                struct aws_byte_cursor to_read = aws_byte_cursor_advance(data, bytes_to_read);
                bool succ = aws_byte_buf_write_from_whole_cursor(&decoder->scratch, to_read);
                AWS_ASSERT(succ);
                (void)succ;
            }

            /* If we have the correct number of bytes, call the state */
            if (will_finish_state) {

                DECODER_LOGF(TRACE, decoder, "Running state '%s' (using scratch)", current_state_name);

                struct aws_byte_cursor state_data = aws_byte_cursor_from_buf(&decoder->scratch);
                err = decoder->state->fn(decoder, &state_data);
                if (aws_h2err_failed(err)) {
                    goto handle_error;
                }

                AWS_ASSERT(state_data.len == 0 && "Decoder state requested more data than it used");
            } else {
                DECODER_LOGF(
                    TRACE,
                    decoder,
                    "State '%s' requires %" PRIu32 " bytes, but only %zu available, trying again later",
                    current_state_name,
                    bytes_required,
                    decoder->scratch.len);
            }
        }
    } while (decoder->state_changed);

    return AWS_H2ERR_SUCCESS;

handle_error:
    decoder->has_errored = true;
    return err;
}

/***********************************************************************************************************************
 * State functions
 **********************************************************************************************************************/

static struct aws_h2err s_decoder_switch_state(struct aws_h2_decoder *decoder, const struct h2_decoder_state *state) {
    /* Ensure payload is big enough to enter next state.
     * If this fails, then the payload length we received is too small for this frame type.
     * (ex: a RST_STREAM frame with < 4 bytes) */
    if (decoder->frame_in_progress.payload_len < state->bytes_required) {
        DECODER_LOGF(
            ERROR, decoder, "%s payload is too small", aws_h2_frame_type_to_str(decoder->frame_in_progress.type));
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FRAME_SIZE_ERROR);
    }

    DECODER_LOGF(TRACE, decoder, "Moving from state '%s' to '%s'", decoder->state->name, state->name);
    decoder->scratch.len = 0;
    decoder->state = state;
    decoder->state_changed = true;
    return AWS_H2ERR_SUCCESS;
}

static struct aws_h2err s_decoder_switch_to_frame_state(struct aws_h2_decoder *decoder) {
    AWS_ASSERT(decoder->frame_in_progress.type < AWS_H2_FRAME_TYPE_COUNT);
    return s_decoder_switch_state(decoder, s_state_frames[decoder->frame_in_progress.type]);
}

static struct aws_h2err s_decoder_reset_state(struct aws_h2_decoder *decoder) {
    /* Ensure we've consumed all payload (and padding) when state machine finishes this frame.
     * If this fails, the payload length we received is too large for this frame type.
     * (ex: a RST_STREAM frame with > 4 bytes) */
    if (decoder->frame_in_progress.payload_len > 0 || decoder->frame_in_progress.padding_len > 0) {
        DECODER_LOGF(
            ERROR, decoder, "%s frame payload is too large", aws_h2_frame_type_to_str(decoder->frame_in_progress.type));
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FRAME_SIZE_ERROR);
    }

    DECODER_LOGF(TRACE, decoder, "%s frame complete", aws_h2_frame_type_to_str(decoder->frame_in_progress.type));

    decoder->scratch.len = 0;
    decoder->state = &s_state_prefix;
    decoder->state_changed = true;

    AWS_ZERO_STRUCT(decoder->frame_in_progress);
    return AWS_H2ERR_SUCCESS;
}

/* Returns as much of the current frame's payload as possible, and updates payload_len */
static struct aws_byte_cursor s_decoder_get_payload(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    struct aws_byte_cursor result;

    const uint32_t remaining_length = decoder->frame_in_progress.payload_len;
    if (input->len < remaining_length) {
        AWS_ASSERT(input->len <= UINT32_MAX);
        result = aws_byte_cursor_advance(input, input->len);
    } else {
        result = aws_byte_cursor_advance(input, remaining_length);
    }

    decoder->frame_in_progress.payload_len -= (uint32_t)result.len;

    return result;
}

/* clang-format off */

/* Mask of flags supported by each frame type.
 * Frames not listed have mask of 0, which means all flags will be ignored. */
static const uint8_t s_acceptable_flags_for_frame[AWS_H2_FRAME_TYPE_COUNT] = {
    [AWS_H2_FRAME_T_DATA]           = AWS_H2_FRAME_F_END_STREAM | AWS_H2_FRAME_F_PADDED,
    [AWS_H2_FRAME_T_HEADERS]        = AWS_H2_FRAME_F_END_STREAM | AWS_H2_FRAME_F_END_HEADERS |
                                      AWS_H2_FRAME_F_PADDED | AWS_H2_FRAME_F_PRIORITY,
    [AWS_H2_FRAME_T_PRIORITY]       = 0,
    [AWS_H2_FRAME_T_RST_STREAM]     = 0,
    [AWS_H2_FRAME_T_SETTINGS]       = AWS_H2_FRAME_F_ACK,
    [AWS_H2_FRAME_T_PUSH_PROMISE]   = AWS_H2_FRAME_F_END_HEADERS | AWS_H2_FRAME_F_PADDED,
    [AWS_H2_FRAME_T_PING]           = AWS_H2_FRAME_F_ACK,
    [AWS_H2_FRAME_T_GOAWAY]         = 0,
    [AWS_H2_FRAME_T_WINDOW_UPDATE]  = 0,
    [AWS_H2_FRAME_T_CONTINUATION]   = AWS_H2_FRAME_F_END_HEADERS,
    [AWS_H2_FRAME_T_UNKNOWN]        = 0,
};

enum stream_id_rules {
    STREAM_ID_REQUIRED,
    STREAM_ID_FORBIDDEN,
    STREAM_ID_EITHER_WAY,
};

/* Frame-types generally either require a stream-id, or require that it be zero. */
static const enum stream_id_rules s_stream_id_rules_for_frame[AWS_H2_FRAME_TYPE_COUNT] = {
    [AWS_H2_FRAME_T_DATA]           = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_HEADERS]        = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_PRIORITY]       = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_RST_STREAM]     = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_SETTINGS]       = STREAM_ID_FORBIDDEN,
    [AWS_H2_FRAME_T_PUSH_PROMISE]   = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_PING]           = STREAM_ID_FORBIDDEN,
    [AWS_H2_FRAME_T_GOAWAY]         = STREAM_ID_FORBIDDEN,
    [AWS_H2_FRAME_T_WINDOW_UPDATE]  = STREAM_ID_EITHER_WAY, /* WINDOW_UPDATE is special and can do either */
    [AWS_H2_FRAME_T_CONTINUATION]   = STREAM_ID_REQUIRED,
    [AWS_H2_FRAME_T_UNKNOWN]        = STREAM_ID_EITHER_WAY, /* Everything in an UNKNOWN frame type is ignored */
};
/* clang-format on */

/* All frames begin with a fixed 9-octet header followed by a variable-length payload. (RFC-7540 4.1)
 * This function processes everything preceding Frame Payload in the following diagram:
 *  +-----------------------------------------------+
 *  |                 Length (24)                   |
 *  +---------------+---------------+---------------+
 *  |   Type (8)    |   Flags (8)   |
 *  +-+-------------+---------------+-------------------------------+
 *  |R|                 Stream Identifier (31)                      |
 *  +=+=============================================================+
 *  |                   Frame Payload (0...)                      ...
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_prefix(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_prefix_requires_9_bytes);

    struct aws_frame_in_progress *frame = &decoder->frame_in_progress;
    uint8_t raw_type = 0;
    uint8_t raw_flags = 0;

    /* Read the raw values from the first 9 bytes */
    bool all_read = true;
    all_read &= aws_byte_cursor_read_be24(input, &frame->payload_len);
    all_read &= aws_byte_cursor_read_u8(input, &raw_type);
    all_read &= aws_byte_cursor_read_u8(input, &raw_flags);
    all_read &= aws_byte_cursor_read_be32(input, &frame->stream_id);
    AWS_ASSERT(all_read);
    (void)all_read;

    /* Validate frame type */
    frame->type = raw_type < AWS_H2_FRAME_T_UNKNOWN ? raw_type : AWS_H2_FRAME_T_UNKNOWN;

    /* Validate the frame's flags
     * Flags that have no defined semantics for a particular frame type MUST be ignored (RFC-7540 4.1) */
    const uint8_t flags = raw_flags & s_acceptable_flags_for_frame[decoder->frame_in_progress.type];

    bool is_padded = flags & AWS_H2_FRAME_F_PADDED;
    decoder->frame_in_progress.flags.ack = flags & AWS_H2_FRAME_F_ACK;
    decoder->frame_in_progress.flags.end_stream = flags & AWS_H2_FRAME_F_END_STREAM;
    decoder->frame_in_progress.flags.end_headers = flags & AWS_H2_FRAME_F_END_HEADERS;
    decoder->frame_in_progress.flags.priority =
        flags & AWS_H2_FRAME_F_PRIORITY || decoder->frame_in_progress.type == AWS_H2_FRAME_T_PRIORITY;

    /* Connection preface requires that SETTINGS be sent first (RFC-7540 3.5).
     * This should be the first error we check for, so that a connection sending
     * total garbage data is likely to trigger this PROTOCOL_ERROR */
    if (!decoder->connection_preface_complete) {
        if (frame->type == AWS_H2_FRAME_T_SETTINGS && !frame->flags.ack) {
            DECODER_LOG(TRACE, decoder, "Connection preface satisfied.");
            decoder->connection_preface_complete = true;
        } else {
            DECODER_LOG(ERROR, decoder, "First frame must be SETTINGS");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
    }

    /* Validate the frame's stream ID. */

    /* Reserved bit (1st bit) MUST be ignored when receiving (RFC-7540 4.1) */
    frame->stream_id &= s_31_bit_mask;

    /* Some frame types require a stream ID, some frame types require that stream ID be zero. */
    const enum stream_id_rules stream_id_rules = s_stream_id_rules_for_frame[frame->type];
    if (frame->stream_id) {
        if (stream_id_rules == STREAM_ID_FORBIDDEN) {
            DECODER_LOGF(ERROR, decoder, "Stream ID for %s frame must be 0.", aws_h2_frame_type_to_str(frame->type));
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
    } else {
        if (stream_id_rules == STREAM_ID_REQUIRED) {
            DECODER_LOGF(ERROR, decoder, "Stream ID for %s frame cannot be 0.", aws_h2_frame_type_to_str(frame->type));
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
    }

    /* A header-block starts with a HEADERS or PUSH_PROMISE frame, followed by 0 or more CONTINUATION frames.
     * It's an error for any other frame-type or stream ID to arrive while a header-block is in progress.
     * (RFC-7540 4.3) */
    if (frame->type == AWS_H2_FRAME_T_CONTINUATION) {
        if (decoder->header_block_in_progress.stream_id != frame->stream_id) {
            DECODER_LOG(ERROR, decoder, "Unexpected CONTINUATION frame.");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
    } else {
        if (decoder->header_block_in_progress.stream_id) {
            DECODER_LOG(ERROR, decoder, "Expected CONTINUATION frame.");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
    }

    /* Validate payload length.  */
    uint32_t max_frame_size = decoder->settings.max_frame_size;
    if (frame->payload_len > max_frame_size) {
        DECODER_LOGF(
            ERROR,
            decoder,
            "Decoder's max frame size is %" PRIu32 ", but frame of size %" PRIu32 " was received.",
            max_frame_size,
            frame->payload_len);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FRAME_SIZE_ERROR);
    }

    DECODER_LOGF(
        TRACE,
        decoder,
        "Done decoding frame prefix (type=%s stream-id=%" PRIu32 " payload-len=%" PRIu32 "), moving on to payload",
        aws_h2_frame_type_to_str(frame->type),
        frame->stream_id,
        frame->payload_len);

    if (is_padded) {
        /* Read padding length if necessary */
        return s_decoder_switch_state(decoder, &s_state_padding_len);
    }
    if (decoder->frame_in_progress.type == AWS_H2_FRAME_T_DATA) {
        /* We invoke the on_data_begin here to report the whole payload size */
        DECODER_CALL_VTABLE_STREAM_ARGS(
            decoder, on_data_begin, frame->payload_len, 0 /*padding_len*/, frame->flags.end_stream);
    }
    if (decoder->frame_in_progress.flags.priority) {
        /* Read the stream dependency and weight if PRIORITY is set */
        return s_decoder_switch_state(decoder, &s_state_priority_block);
    }

    /* Set the state to the appropriate frame's state */
    return s_decoder_switch_to_frame_state(decoder);
}

/* Frames that support padding, and have the PADDED flag set, begin with a 1-byte Pad Length.
 * (Actual padding comes later at the very end of the frame)
 *  +---------------+
 *  |Pad Length? (8)|
 *  +---------------+
 */
static struct aws_h2err s_state_fn_padding_len(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_padding_len_requires_1_bytes);

    struct aws_frame_in_progress *frame = &decoder->frame_in_progress;
    /* Read the padding length */
    bool succ = aws_byte_cursor_read_u8(input, &frame->padding_len);
    AWS_ASSERT(succ);
    (void)succ;

    /* Adjust payload size so it doesn't include padding (or the 1-byte padding length) */
    uint32_t reduce_payload = s_state_padding_len_requires_1_bytes + frame->padding_len;
    if (reduce_payload > decoder->frame_in_progress.payload_len) {
        DECODER_LOG(ERROR, decoder, "Padding length exceeds payload length");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    if (frame->type == AWS_H2_FRAME_T_DATA) {
        /* We invoke the on_data_begin here to report the whole payload size and the padding size */
        DECODER_CALL_VTABLE_STREAM_ARGS(
            decoder, on_data_begin, frame->payload_len, frame->padding_len + 1, frame->flags.end_stream);
    }

    frame->payload_len -= reduce_payload;

    DECODER_LOGF(TRACE, decoder, "Padding length of frame: %" PRIu32, frame->padding_len);
    if (frame->flags.priority) {
        /* Read the stream dependency and weight if PRIORITY is set */
        return s_decoder_switch_state(decoder, &s_state_priority_block);
    }

    /* Set the state to the appropriate frame's state */
    return s_decoder_switch_to_frame_state(decoder);
}

static struct aws_h2err s_state_fn_padding(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    const uint8_t remaining_len = decoder->frame_in_progress.padding_len;
    const uint8_t consuming_len = input->len < remaining_len ? (uint8_t)input->len : remaining_len;
    aws_byte_cursor_advance(input, consuming_len);
    decoder->frame_in_progress.padding_len -= consuming_len;

    if (remaining_len == consuming_len) {
        /* Done with the frame! */
        return s_decoder_reset_state(decoder);
    }

    return AWS_H2ERR_SUCCESS;
}

/* Shared code for:
 * PRIORITY frame (RFC-7540 6.3)
 * Start of HEADERS frame IF the priority flag is set (RFC-7540 6.2)
 *  +-+-------------+-----------------------------------------------+
 *  |E|                 Stream Dependency (31)                      |
 *  +-+-------------+-----------------------------------------------+
 *  |  Weight (8)   |
 *  +-+-------------+-----------------------------------------------+
 */
static struct aws_h2err s_state_fn_priority_block(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_priority_block_requires_5_bytes);

    /* #NOTE: throw priority data on the GROUND. They make us hecka vulnerable to DDoS and stuff.
     * https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2019-9513
     */
    aws_byte_cursor_advance(input, s_state_priority_block_requires_5_bytes);

    decoder->frame_in_progress.payload_len -= s_state_priority_block_requires_5_bytes;

    return s_decoder_switch_to_frame_state(decoder);
}

static struct aws_h2err s_state_fn_frame_data(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    const struct aws_byte_cursor body_data = s_decoder_get_payload(decoder, input);

    if (body_data.len) {
        DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_data_i, body_data);
    }

    if (decoder->frame_in_progress.payload_len == 0) {
        DECODER_CALL_VTABLE_STREAM(decoder, on_data_end);
        /* If frame had END_STREAM flag, alert user now */
        if (decoder->frame_in_progress.flags.end_stream) {
            DECODER_CALL_VTABLE_STREAM(decoder, on_end_stream);
        }

        /* Process padding if necessary, otherwise we're done! */
        return s_decoder_switch_state(decoder, &s_state_padding);
    }

    return AWS_H2ERR_SUCCESS;
}
static struct aws_h2err s_state_fn_frame_headers(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    /* Start header-block and alert the user */
    decoder->header_block_in_progress.stream_id = decoder->frame_in_progress.stream_id;
    decoder->header_block_in_progress.is_push_promise = false;
    decoder->header_block_in_progress.ends_stream = decoder->frame_in_progress.flags.end_stream;

    DECODER_CALL_VTABLE_STREAM(decoder, on_headers_begin);

    /* Read the header-block fragment */
    return s_decoder_switch_state(decoder, &s_state_header_block_loop);
}
static struct aws_h2err s_state_fn_frame_priority(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    /* We already processed this data in the shared priority_block state, so we're done! */
    return s_decoder_reset_state(decoder);
}

/*  RST_STREAM is just a 4-byte error code.
 *  +---------------------------------------------------------------+
 *  |                        Error Code (32)                        |
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_rst_stream(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_frame_rst_stream_requires_4_bytes);

    uint32_t error_code = 0;
    bool succ = aws_byte_cursor_read_be32(input, &error_code);
    AWS_ASSERT(succ);
    (void)succ;

    decoder->frame_in_progress.payload_len -= s_state_frame_rst_stream_requires_4_bytes;

    DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_rst_stream, error_code);

    return s_decoder_reset_state(decoder);
}

/* A SETTINGS frame may contain any number of 6-byte entries.
 * This state consumes no data, but sends us into the appropriate next state */
static struct aws_h2err s_state_fn_frame_settings_begin(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    /* If ack is set, report and we're done */
    if (decoder->frame_in_progress.flags.ack) {
        /* Receipt of a SETTINGS frame with the ACK flag set and a length field value other
         * than 0 MUST be treated as a connection error of type FRAME_SIZE_ERROR */
        if (decoder->frame_in_progress.payload_len) {
            DECODER_LOGF(
                ERROR,
                decoder,
                "SETTINGS ACK frame received, but it has non-0 payload length %" PRIu32,
                decoder->frame_in_progress.payload_len);
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FRAME_SIZE_ERROR);
        }

        DECODER_CALL_VTABLE(decoder, on_settings_ack);
        return s_decoder_reset_state(decoder);
    }

    if (decoder->frame_in_progress.payload_len % s_state_frame_settings_i_requires_6_bytes != 0) {
        /* A SETTINGS frame with a length other than a multiple of 6 octets MUST be
         * treated as a connection error (Section 5.4.1) of type FRAME_SIZE_ERROR */
        DECODER_LOGF(
            ERROR,
            decoder,
            "Settings frame payload length is %" PRIu32 ", but it must be divisible by %" PRIu32,
            decoder->frame_in_progress.payload_len,
            s_state_frame_settings_i_requires_6_bytes);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FRAME_SIZE_ERROR);
    }

    /* Enter looping states until all entries are consumed. */
    return s_decoder_switch_state(decoder, &s_state_frame_settings_loop);
}

/* Check if we're done consuming settings */
static struct aws_h2err s_state_fn_frame_settings_loop(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    if (decoder->frame_in_progress.payload_len == 0) {
        /* Huzzah, done with the frame, fire the callback */
        struct aws_array_list *buffer = &decoder->settings_buffer_list;
        DECODER_CALL_VTABLE_ARGS(
            decoder, on_settings, buffer->data, aws_array_list_length(&decoder->settings_buffer_list));
        /* clean up the buffer */
        aws_array_list_clear(&decoder->settings_buffer_list);
        return s_decoder_reset_state(decoder);
    }

    return s_decoder_switch_state(decoder, &s_state_frame_settings_i);
}

/* Each run through this state consumes one 6-byte setting.
 * There may be multiple settings in a SETTINGS frame.
 *  +-------------------------------+
 *  |       Identifier (16)         |
 *  +-------------------------------+-------------------------------+
 *  |                        Value (32)                             |
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_settings_i(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_frame_settings_i_requires_6_bytes);

    uint16_t id = 0;
    uint32_t value = 0;

    bool succ = aws_byte_cursor_read_be16(input, &id);
    AWS_ASSERT(succ);
    (void)succ;

    succ = aws_byte_cursor_read_be32(input, &value);
    AWS_ASSERT(succ);
    (void)succ;

    /* An endpoint that receives a SETTINGS frame with any unknown or unsupported identifier MUST ignore that setting.
     * RFC-7540 6.5.2 */
    if (id >= AWS_HTTP2_SETTINGS_BEGIN_RANGE && id < AWS_HTTP2_SETTINGS_END_RANGE) {
        /* check the value meets the settings bounds */
        if (value < aws_h2_settings_bounds[id][0] || value > aws_h2_settings_bounds[id][1]) {
            DECODER_LOGF(
                ERROR, decoder, "A value of SETTING frame is invalid, id: %" PRIu16 ", value: %" PRIu32, id, value);
            if (id == AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE) {
                return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR);
            } else {
                return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
            }
        }
        struct aws_http2_setting setting;
        setting.id = id;
        setting.value = value;
        /* array_list will keep a copy of setting, it is fine to be a local variable */
        if (aws_array_list_push_back(&decoder->settings_buffer_list, &setting)) {
            DECODER_LOGF(ERROR, decoder, "Writing setting to buffer failed, %s", aws_error_name(aws_last_error()));
            return aws_h2err_from_last_error();
        }
    }

    /* Update payload len */
    decoder->frame_in_progress.payload_len -= s_state_frame_settings_i_requires_6_bytes;

    return s_decoder_switch_state(decoder, &s_state_frame_settings_loop);
}

/* Read 4-byte Promised Stream ID
 * The rest of the frame is just like HEADERS, so move on to shared states...
 *  +-+-------------------------------------------------------------+
 *  |R|                  Promised Stream ID (31)                    |
 *  +-+-----------------------------+-------------------------------+
 */
static struct aws_h2err s_state_fn_frame_push_promise(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    if (decoder->settings.enable_push == 0) {
        /* treat the receipt of a PUSH_PROMISE frame as a connection error of type PROTOCOL_ERROR.(RFC-7540 6.5.2) */
        DECODER_LOG(ERROR, decoder, "PUSH_PROMISE is invalid, the seting for enable push is 0");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    AWS_ASSERT(input->len >= s_state_frame_push_promise_requires_4_bytes);

    uint32_t promised_stream_id = 0;
    bool succ = aws_byte_cursor_read_be32(input, &promised_stream_id);
    AWS_ASSERT(succ);
    (void)succ;

    decoder->frame_in_progress.payload_len -= s_state_frame_push_promise_requires_4_bytes;

    /* Reserved bit (top bit) must be ignored when receiving (RFC-7540 4.1) */
    promised_stream_id &= s_31_bit_mask;

    /* Promised stream ID must not be 0 (RFC-7540 6.6).
     * Promised stream ID (server-initiated) must be even-numbered (RFC-7540 5.1.1). */
    if ((promised_stream_id == 0) || (promised_stream_id % 2) != 0) {
        DECODER_LOGF(ERROR, decoder, "PUSH_PROMISE is promising invalid stream ID %" PRIu32, promised_stream_id);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    /* Server cannot receive PUSH_PROMISE frames */
    if (decoder->is_server) {
        DECODER_LOG(ERROR, decoder, "Server cannot receive PUSH_PROMISE frames");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    /* Start header-block and alert the user. */
    decoder->header_block_in_progress.stream_id = decoder->frame_in_progress.stream_id;
    decoder->header_block_in_progress.is_push_promise = true;
    decoder->header_block_in_progress.ends_stream = false;

    DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_push_promise_begin, promised_stream_id);

    /* Read the header-block fragment */
    return s_decoder_switch_state(decoder, &s_state_header_block_loop);
}

/* PING frame is just 8-bytes of opaque data.
 *  +---------------------------------------------------------------+
 *  |                                                               |
 *  |                      Opaque Data (64)                         |
 *  |                                                               |
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_ping(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_frame_ping_requires_8_bytes);

    uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE] = {0};
    bool succ = aws_byte_cursor_read(input, &opaque_data, AWS_HTTP2_PING_DATA_SIZE);
    AWS_ASSERT(succ);
    (void)succ;

    decoder->frame_in_progress.payload_len -= s_state_frame_ping_requires_8_bytes;

    if (decoder->frame_in_progress.flags.ack) {
        DECODER_CALL_VTABLE_ARGS(decoder, on_ping_ack, opaque_data);
    } else {
        DECODER_CALL_VTABLE_ARGS(decoder, on_ping, opaque_data);
    }

    return s_decoder_reset_state(decoder);
}

/* Read first 8 bytes of GOAWAY.
 * This may be followed by N bytes of debug data.
 *  +-+-------------------------------------------------------------+
 *  |R|                  Last-Stream-ID (31)                        |
 *  +-+-------------------------------------------------------------+
 *  |                      Error Code (32)                          |
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_goaway(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_frame_goaway_requires_8_bytes);

    uint32_t last_stream = 0;
    uint32_t error_code = AWS_HTTP2_ERR_NO_ERROR;

    bool succ = aws_byte_cursor_read_be32(input, &last_stream);
    AWS_ASSERT(succ);
    (void)succ;

    last_stream &= s_31_bit_mask;

    succ = aws_byte_cursor_read_be32(input, &error_code);
    AWS_ASSERT(succ);
    (void)succ;

    decoder->frame_in_progress.payload_len -= s_state_frame_goaway_requires_8_bytes;
    uint32_t debug_data_length = decoder->frame_in_progress.payload_len;
    /* Received new GOAWAY, clean up the previous one. Buffer it up and invoke the callback once the debug data decoded
     * fully. */
    decoder->goaway_in_progress.error_code = error_code;
    decoder->goaway_in_progress.last_stream = last_stream;
    int init_result = aws_byte_buf_init(&decoder->goaway_in_progress.debug_data, decoder->alloc, debug_data_length);
    AWS_ASSERT(init_result == 0);
    (void)init_result;

    return s_decoder_switch_state(decoder, &s_state_frame_goaway_debug_data);
}

/* Optional remainder of GOAWAY frame.
 *  +---------------------------------------------------------------+
 *  |                  Additional Debug Data (*)                    |
 *  +---------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_goaway_debug_data(
    struct aws_h2_decoder *decoder,
    struct aws_byte_cursor *input) {

    struct aws_byte_cursor debug_data = s_decoder_get_payload(decoder, input);
    if (debug_data.len > 0) {
        /* As we initialized the buffer to the size of debug data, we can safely append here */
        aws_byte_buf_append(&decoder->goaway_in_progress.debug_data, &debug_data);
    }

    /* If this is the last data in the frame, reset decoder */
    if (decoder->frame_in_progress.payload_len == 0) {
        struct aws_byte_cursor debug_cursor = aws_byte_cursor_from_buf(&decoder->goaway_in_progress.debug_data);

        DECODER_CALL_VTABLE_ARGS(
            decoder,
            on_goaway,
            decoder->goaway_in_progress.last_stream,
            decoder->goaway_in_progress.error_code,
            debug_cursor);
        aws_byte_buf_clean_up(&decoder->goaway_in_progress.debug_data);
        return s_decoder_reset_state(decoder);
    }

    return AWS_H2ERR_SUCCESS;
}

/* WINDOW_UPDATE frame.
 *  +-+-------------------------------------------------------------+
 *  |R|              Window Size Increment (31)                     |
 *  +-+-------------------------------------------------------------+
 */
static struct aws_h2err s_state_fn_frame_window_update(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    AWS_ASSERT(input->len >= s_state_frame_window_update_requires_4_bytes);

    uint32_t window_increment = 0;
    bool succ = aws_byte_cursor_read_be32(input, &window_increment);
    AWS_ASSERT(succ);
    (void)succ;

    decoder->frame_in_progress.payload_len -= s_state_frame_window_update_requires_4_bytes;

    window_increment &= s_31_bit_mask;

    DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_window_update, window_increment);

    return s_decoder_reset_state(decoder);
}

/* CONTINUATION is a lot like HEADERS, so it uses shared states. */
static struct aws_h2err s_state_fn_frame_continuation(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    /* Read the header-block fragment */
    return s_decoder_switch_state(decoder, &s_state_header_block_loop);
}

/* Implementations MUST ignore and discard any frame that has a type that is unknown. */
static struct aws_h2err s_state_fn_frame_unknown(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {

    /* Read all data possible, and throw it on the floor */
    s_decoder_get_payload(decoder, input);

    /* If there's no more data expected, end the frame */
    if (decoder->frame_in_progress.payload_len == 0) {
        return s_decoder_reset_state(decoder);
    }

    return AWS_H2ERR_SUCCESS;
}

/* Perform analysis that can't be done until all pseudo-headers are received.
 * Then deliver buffered pseudoheaders via callback */
static struct aws_h2err s_flush_pseudoheaders(struct aws_h2_decoder *decoder) {
    struct aws_header_block_in_progress *current_block = &decoder->header_block_in_progress;

    if (current_block->malformed) {
        goto already_malformed;
    }

    if (current_block->pseudoheaders_done) {
        return AWS_H2ERR_SUCCESS;
    }
    current_block->pseudoheaders_done = true;

    /* s_process_header_field() already checked that we're not mixing request & response pseudoheaders */
    bool has_request_pseudoheaders = false;
    for (int i = PSEUDOHEADER_METHOD; i <= PSEUDOHEADER_PATH; ++i) {
        if (current_block->pseudoheader_values[i] != NULL) {
            has_request_pseudoheaders = true;
            break;
        }
    }

    bool has_response_pseudoheaders = current_block->pseudoheader_values[PSEUDOHEADER_STATUS] != NULL;

    if (current_block->is_push_promise && !has_request_pseudoheaders) {
        DECODER_LOG(ERROR, decoder, "PUSH_PROMISE is missing :method");
        goto malformed;
    }

    if (has_request_pseudoheaders) {
        /* Request header-block. */
        current_block->block_type = AWS_HTTP_HEADER_BLOCK_MAIN;

    } else if (has_response_pseudoheaders) {
        /* Response header block. */

        /* Determine whether this is an Informational (1xx) response */
        struct aws_byte_cursor status_value =
            aws_byte_cursor_from_string(current_block->pseudoheader_values[PSEUDOHEADER_STATUS]);
        uint64_t status_code;
        if (status_value.len != 3 || aws_byte_cursor_utf8_parse_u64(status_value, &status_code)) {
            DECODER_LOG(ERROR, decoder, ":status header has invalid value");
            DECODER_LOGF(DEBUG, decoder, "Bad :status value is '" PRInSTR "'", AWS_BYTE_CURSOR_PRI(status_value));
            goto malformed;
        }

        if (status_code / 100 == 1) {
            current_block->block_type = AWS_HTTP_HEADER_BLOCK_INFORMATIONAL;

            if (current_block->ends_stream) {
                /* Informational headers do not constitute a full response (RFC-7540 8.1) */
                DECODER_LOG(ERROR, decoder, "Informational (1xx) response cannot END_STREAM");
                goto malformed;
            }
            current_block->body_headers_forbidden = true;
        } else {
            current_block->block_type = AWS_HTTP_HEADER_BLOCK_MAIN;
        }
        /**
         * RFC-9110 8.6.
         * A server MUST NOT send a Content-Length header field in any response with a status code of 1xx
         * (Informational) or 204 (No Content).
         */
        current_block->body_headers_forbidden |= status_code == AWS_HTTP_STATUS_CODE_204_NO_CONTENT;

    } else {
        /* Trailing header block. */
        if (!current_block->ends_stream) {
            DECODER_LOG(ERROR, decoder, "HEADERS appear to be trailer, but lack END_STREAM");
            goto malformed;
        }

        current_block->block_type = AWS_HTTP_HEADER_BLOCK_TRAILING;
    }

    /* #TODO RFC-7540 8.1.2.3 & 8.3 Validate request has correct pseudoheaders. Note different rules for CONNECT */
    /* #TODO validate pseudoheader values. each one has its own special rules */

    /* Finally, deliver header-fields via callback */
    for (size_t i = 0; i < PSEUDOHEADER_COUNT; ++i) {
        const struct aws_string *value_string = current_block->pseudoheader_values[i];
        if (value_string) {

            struct aws_http_header header_field = {
                .name = *s_pseudoheader_name_to_cursor[i],
                .value = aws_byte_cursor_from_string(value_string),
                .compression = current_block->pseudoheader_compression[i],
            };

            enum aws_http_header_name name_enum = s_pseudoheader_to_header_name[i];

            if (current_block->is_push_promise) {
                DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_push_promise_i, &header_field, name_enum);
            } else {
                DECODER_CALL_VTABLE_STREAM_ARGS(
                    decoder, on_headers_i, &header_field, name_enum, current_block->block_type);
            }
        }
    }

    return AWS_H2ERR_SUCCESS;

malformed:
    /* A malformed header-block is not a connection error, it's a Stream Error (RFC-7540 5.4.2).
     * We continue decoding and report that it's malformed in on_headers_end(). */
    current_block->malformed = true;
    return AWS_H2ERR_SUCCESS;
already_malformed:
    return AWS_H2ERR_SUCCESS;
}

/* Process single header-field.
 * If it's invalid, mark the header-block as malformed.
 * If it's valid, and header-block is not malformed, deliver via callback. */
static struct aws_h2err s_process_header_field(
    struct aws_h2_decoder *decoder,
    const struct aws_http_header *header_field) {

    struct aws_header_block_in_progress *current_block = &decoder->header_block_in_progress;
    if (current_block->malformed) {
        goto already_malformed;
    }

    const struct aws_byte_cursor name = header_field->name;
    if (name.len == 0) {
        DECODER_LOG(ERROR, decoder, "Header name is blank");
        goto malformed;
    }

    enum aws_http_header_name name_enum = aws_http_lowercase_str_to_header_name(name);

    bool is_pseudoheader = name.ptr[0] == ':';
    if (is_pseudoheader) {
        if (current_block->pseudoheaders_done) {
            /* Note: being careful not to leak possibly sensitive data except at DEBUG level and lower */
            DECODER_LOG(ERROR, decoder, "Pseudo-headers must appear before regular fields.");
            DECODER_LOGF(DEBUG, decoder, "Misplaced pseudo-header is '" PRInSTR "'", AWS_BYTE_CURSOR_PRI(name));
            goto malformed;
        }

        enum pseudoheader_name pseudoheader_enum = s_header_to_pseudoheader_name(name_enum);
        if (pseudoheader_enum == PSEUDOHEADER_UNKNOWN) {
            DECODER_LOG(ERROR, decoder, "Unrecognized pseudo-header");
            DECODER_LOGF(DEBUG, decoder, "Unrecognized pseudo-header is '" PRInSTR "'", AWS_BYTE_CURSOR_PRI(name));
            goto malformed;
        }

        /* Ensure request pseudo-headers vs response pseudoheaders were sent appropriately.
         * This also ensures that request and response pseudoheaders aren't being mixed. */
        bool expect_request_pseudoheader = decoder->is_server || current_block->is_push_promise;
        bool is_request_pseudoheader = pseudoheader_enum != PSEUDOHEADER_STATUS;
        if (expect_request_pseudoheader != is_request_pseudoheader) {
            DECODER_LOGF(
                ERROR, /* ok to log name of recognized pseudo-header at ERROR level */
                decoder,
                "'" PRInSTR "' pseudo-header cannot be in %s header-block to %s",
                AWS_BYTE_CURSOR_PRI(name),
                current_block->is_push_promise ? "PUSH_PROMISE" : "HEADERS",
                decoder->is_server ? "server" : "client");
            goto malformed;
        }

        /* Protect against duplicates. */
        if (current_block->pseudoheader_values[pseudoheader_enum] != NULL) {
            /* ok to log name of recognized pseudo-header at ERROR level */
            DECODER_LOGF(
                ERROR, decoder, "'" PRInSTR "' pseudo-header occurred multiple times", AWS_BYTE_CURSOR_PRI(name));
            goto malformed;
        }

        /* Buffer up pseudo-headers, we'll deliver them later once they're all validated. */
        current_block->pseudoheader_compression[pseudoheader_enum] = header_field->compression;
        current_block->pseudoheader_values[pseudoheader_enum] =
            aws_string_new_from_cursor(decoder->alloc, &header_field->value);
        if (!current_block->pseudoheader_values[pseudoheader_enum]) {
            return aws_h2err_from_last_error();
        }

    } else { /* Else regular header-field. */

        /* Regular header-fields come after pseudo-headers, so make sure pseudo-headers are flushed */
        if (!current_block->pseudoheaders_done) {
            struct aws_h2err err = s_flush_pseudoheaders(decoder);
            if (aws_h2err_failed(err)) {
                return err;
            }

            /* might have realized that header-block is malformed during flush */
            if (current_block->malformed) {
                goto already_malformed;
            }
        }

        /* Validate header name (not necessary if string already matched against a known enum) */
        if (name_enum == AWS_HTTP_HEADER_UNKNOWN) {
            if (!aws_strutil_is_lowercase_http_token(name)) {
                DECODER_LOG(ERROR, decoder, "Header name contains invalid characters");
                DECODER_LOGF(DEBUG, decoder, "Bad header name is '" PRInSTR "'", AWS_BYTE_CURSOR_PRI(name));
                goto malformed;
            }
        }

        /* #TODO Validate characters used in header_field->value */

        switch (name_enum) {
            case AWS_HTTP_HEADER_COOKIE:
                /* for a header cookie, we will not fire callback until we concatenate them all, let's store it at the
                 * buffer */
                if (header_field->compression > current_block->cookie_header_compression_type) {
                    current_block->cookie_header_compression_type = header_field->compression;
                }

                if (current_block->cookies.len) {
                    /* add a delimiter */
                    struct aws_byte_cursor delimiter = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("; ");
                    if (aws_byte_buf_append_dynamic(&current_block->cookies, &delimiter)) {
                        return aws_h2err_from_last_error();
                    }
                }
                if (aws_byte_buf_append_dynamic(&current_block->cookies, &header_field->value)) {
                    return aws_h2err_from_last_error();
                }
                /* Early return */
                return AWS_H2ERR_SUCCESS;
            case AWS_HTTP_HEADER_TRANSFER_ENCODING:
            case AWS_HTTP_HEADER_UPGRADE:
            case AWS_HTTP_HEADER_KEEP_ALIVE:
            case AWS_HTTP_HEADER_PROXY_CONNECTION: {
                /* connection-specific header field are treated as malformed (RFC9113 8.2.2) */
                DECODER_LOGF(
                    ERROR,
                    decoder,
                    "Connection-specific header ('" PRInSTR "') found, not allowed in HTTP/2",
                    AWS_BYTE_CURSOR_PRI(name));
                goto malformed;
            } break;

            case AWS_HTTP_HEADER_CONTENT_LENGTH:
                if (current_block->body_headers_forbidden) {
                    /* The content-length are forbidden */
                    DECODER_LOG(ERROR, decoder, "Unexpected Content-Length header found");
                    goto malformed;
                }
                break;
            default:
                break;
        }
        /* Deliver header-field via callback */
        if (current_block->is_push_promise) {
            DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_push_promise_i, header_field, name_enum);
        } else {
            DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_headers_i, header_field, name_enum, current_block->block_type);
        }
    }

    return AWS_H2ERR_SUCCESS;

malformed:
    /* A malformed header-block is not a connection error, it's a Stream Error (RFC-7540 5.4.2).
     * We continue decoding and report that it's malformed in on_headers_end(). */
    current_block->malformed = true;
    return AWS_H2ERR_SUCCESS;
already_malformed:
    return AWS_H2ERR_SUCCESS;
}

static struct aws_h2err s_flush_cookie_header(struct aws_h2_decoder *decoder) {
    struct aws_header_block_in_progress *current_block = &decoder->header_block_in_progress;
    if (current_block->malformed) {
        return AWS_H2ERR_SUCCESS;
    }
    if (current_block->cookies.len == 0) {
        /* Nothing to flush */
        return AWS_H2ERR_SUCCESS;
    }
    struct aws_http_header concatenated_cookie;
    struct aws_byte_cursor header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("cookie");
    concatenated_cookie.name = header_name;
    concatenated_cookie.value = aws_byte_cursor_from_buf(&current_block->cookies);
    concatenated_cookie.compression = current_block->cookie_header_compression_type;
    if (current_block->is_push_promise) {
        DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_push_promise_i, &concatenated_cookie, AWS_HTTP_HEADER_COOKIE);
    } else {
        DECODER_CALL_VTABLE_STREAM_ARGS(
            decoder, on_headers_i, &concatenated_cookie, AWS_HTTP_HEADER_COOKIE, current_block->block_type);
    }
    return AWS_H2ERR_SUCCESS;
}

/* This state checks whether we've consumed the current frame's entire header-block fragment.
 * We revisit this state after each entry is decoded.
 * This state consumes no data. */
static struct aws_h2err s_state_fn_header_block_loop(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    (void)input;

    /* If we're out of payload data, handle frame complete */
    if (decoder->frame_in_progress.payload_len == 0) {

        /* If this is the end of the header-block, invoke callback and clear header_block_in_progress */
        if (decoder->frame_in_progress.flags.end_headers) {
            /* Ensure pseudo-headers have been flushed */
            struct aws_h2err err = s_flush_pseudoheaders(decoder);
            if (aws_h2err_failed(err)) {
                return err;
            }
            /* flush the concatenated cookie header */
            err = s_flush_cookie_header(decoder);
            if (aws_h2err_failed(err)) {
                return err;
            }

            bool malformed = decoder->header_block_in_progress.malformed;
            DECODER_LOGF(TRACE, decoder, "Done decoding header-block, malformed=%d", malformed);

            if (decoder->header_block_in_progress.is_push_promise) {
                DECODER_CALL_VTABLE_STREAM_ARGS(decoder, on_push_promise_end, malformed);
            } else {
                DECODER_CALL_VTABLE_STREAM_ARGS(
                    decoder, on_headers_end, malformed, decoder->header_block_in_progress.block_type);
            }

            /* If header-block began with END_STREAM flag, alert user now */
            if (decoder->header_block_in_progress.ends_stream) {
                DECODER_CALL_VTABLE_STREAM(decoder, on_end_stream);
            }

            s_reset_header_block_in_progress(decoder);

        } else {
            DECODER_LOG(TRACE, decoder, "Done decoding header-block fragment, expecting CONTINUATION frames");
        }

        /* Finish this frame */
        return s_decoder_switch_state(decoder, &s_state_padding);
    }

    DECODER_LOGF(
        TRACE,
        decoder,
        "Decoding header-block entry, %" PRIu32 " bytes remaining in payload",
        decoder->frame_in_progress.payload_len);

    return s_decoder_switch_state(decoder, &s_state_header_block_entry);
}

/* We stay in this state until a single "entry" is decoded from the header-block fragment.
 * Then we return to the header_block_loop state */
static struct aws_h2err s_state_fn_header_block_entry(struct aws_h2_decoder *decoder, struct aws_byte_cursor *input) {
    /* This state requires at least 1 byte, but will likely consume more */
    AWS_ASSERT(input->len >= s_state_header_block_entry_requires_1_bytes);

    /* Feed header-block fragment to HPACK decoder.
     * Don't let decoder consume anything beyond payload_len. */
    struct aws_byte_cursor fragment = *input;
    if (fragment.len > decoder->frame_in_progress.payload_len) {
        fragment.len = decoder->frame_in_progress.payload_len;
    }

    const size_t prev_fragment_len = fragment.len;

    struct aws_hpack_decode_result result;
    if (aws_hpack_decode(&decoder->hpack, &fragment, &result)) {
        DECODER_LOGF(ERROR, decoder, "Error decoding header-block fragment: %s", aws_error_name(aws_last_error()));

        /* Any possible error from HPACK decoder (except OOM) is treated as a COMPRESSION error. */
        if (aws_last_error() == AWS_ERROR_OOM) {
            return aws_h2err_from_last_error();
        } else {
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_COMPRESSION_ERROR);
        }
    }

    /* HPACK decoder returns when it reaches the end of an entry, or when it's consumed the whole fragment.
     * Update input & payload_len to reflect the number of bytes consumed. */
    const size_t bytes_consumed = prev_fragment_len - fragment.len;
    aws_byte_cursor_advance(input, bytes_consumed);
    decoder->frame_in_progress.payload_len -= (uint32_t)bytes_consumed;

    if (result.type == AWS_HPACK_DECODE_T_ONGOING) {
        /* HPACK decoder hasn't finished entry */

        if (decoder->frame_in_progress.payload_len > 0) {
            /* More payload is coming. Remain in state until it arrives */
            DECODER_LOG(TRACE, decoder, "Header-block entry partially decoded, waiting for more data.");
            return AWS_H2ERR_SUCCESS;
        }

        if (decoder->frame_in_progress.flags.end_headers) {
            /* Reached end of the frame's payload, and this frame ends the header-block.
             * Error if we ended up with a partially decoded entry. */
            DECODER_LOG(ERROR, decoder, "Compression error: incomplete entry at end of header-block");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_COMPRESSION_ERROR);
        }

        /* Reached end of this frame's payload, but CONTINUATION frames are expected to arrive.
         * We'll resume decoding this entry when we get them. */
        DECODER_LOG(TRACE, decoder, "Header-block entry partially decoded, resumes in CONTINUATION frame");
        return s_decoder_switch_state(decoder, &s_state_header_block_loop);
    }

    /* Finished decoding HPACK entry! */

    /* #TODO Enforces dynamic table resize rules from RFC-7541 4.2
     * If dynamic table size changed via SETTINGS frame, next header-block must start with DYNAMIC_TABLE_RESIZE entry.
     * Is it illegal to receive a resize entry at other times? */

    /* #TODO The TE header field ... MUST NOT contain any value other than "trailers" */

    if (result.type == AWS_HPACK_DECODE_T_HEADER_FIELD) {
        const struct aws_http_header *header_field = &result.data.header_field;

        DECODER_LOGF(
            TRACE,
            decoder,
            "Decoded header field: \"" PRInSTR ": " PRInSTR "\"",
            AWS_BYTE_CURSOR_PRI(header_field->name),
            AWS_BYTE_CURSOR_PRI(header_field->value));

        struct aws_h2err err = s_process_header_field(decoder, header_field);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return s_decoder_switch_state(decoder, &s_state_header_block_loop);
}

/* The first thing a client sends on a connection is a 24 byte magic string (RFC-7540 3.5).
 * Note that this state doesn't "require" the full 24 bytes, it runs as data arrives.
 * This avoids hanging if < 24 bytes rolled in. */
static struct aws_h2err s_state_fn_connection_preface_string(
    struct aws_h2_decoder *decoder,
    struct aws_byte_cursor *input) {
    size_t remaining_len = decoder->connection_preface_cursor.len;
    size_t consuming_len = input->len < remaining_len ? input->len : remaining_len;

    struct aws_byte_cursor expected = aws_byte_cursor_advance(&decoder->connection_preface_cursor, consuming_len);

    struct aws_byte_cursor received = aws_byte_cursor_advance(input, consuming_len);

    if (!aws_byte_cursor_eq(&expected, &received)) {
        DECODER_LOG(ERROR, decoder, "Client connection preface is invalid");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    if (decoder->connection_preface_cursor.len == 0) {
        /* Done receiving connection preface string, proceed to decoding normal frames. */
        return s_decoder_reset_state(decoder);
    }

    /* Remain in state until more data arrives */
    return AWS_H2ERR_SUCCESS;
}

void aws_h2_decoder_set_setting_header_table_size(struct aws_h2_decoder *decoder, uint32_t data) {
    /* Set the protocol_max_size_setting for hpack. */
    aws_hpack_decoder_update_max_table_size(&decoder->hpack, data);
}

void aws_h2_decoder_set_setting_enable_push(struct aws_h2_decoder *decoder, uint32_t data) {
    decoder->settings.enable_push = data;
}

void aws_h2_decoder_set_setting_max_frame_size(struct aws_h2_decoder *decoder, uint32_t data) {
    decoder->settings.max_frame_size = data;
}
