#ifndef AWS_EVENT_STREAM_H_
#define AWS_EVENT_STREAM_H_

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>
#include <aws/common/logging.h>
#include <aws/event-stream/event_stream_exports.h>

#include <stdio.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_C_EVENT_STREAM_PACKAGE_ID 4
/* max message size is 16MB */
#define AWS_EVENT_STREAM_MAX_MESSAGE_SIZE (16 * 1024 * 1024)

/* max header size is 128kb */
#define AWS_EVENT_STREAM_MAX_HEADERS_SIZE (128 * 1024)

/* Max header name length is 127 bytes */
#define AWS_EVENT_STREAM_HEADER_NAME_LEN_MAX (INT8_MAX)

/* Max header static value length is 16 bytes */
#define AWS_EVENT_STREAM_HEADER_STATIC_VALUE_LEN_MAX (16)

enum aws_event_stream_errors {
    AWS_ERROR_EVENT_STREAM_BUFFER_LENGTH_MISMATCH = AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_EVENT_STREAM_PACKAGE_ID),
    AWS_ERROR_EVENT_STREAM_INSUFFICIENT_BUFFER_LEN,
    AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED,
    AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE,
    AWS_ERROR_EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE,
    AWS_ERROR_EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN,
    AWS_ERROR_EVENT_STREAM_MESSAGE_UNKNOWN_HEADER_TYPE,
    AWS_ERROR_EVENT_STREAM_MESSAGE_PARSER_ILLEGAL_STATE,
    AWS_ERROR_EVENT_STREAM_RPC_CONNECTION_CLOSED,
    AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR,
    AWS_ERROR_EVENT_STREAM_RPC_STREAM_CLOSED,
    AWS_ERROR_EVENT_STREAM_RPC_STREAM_NOT_ACTIVATED,

    AWS_ERROR_EVENT_STREAM_END_RANGE = AWS_ERROR_ENUM_END_RANGE(AWS_C_EVENT_STREAM_PACKAGE_ID),
};

enum aws_event_stream_log_subject {
    AWS_LS_EVENT_STREAM_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_EVENT_STREAM_PACKAGE_ID),
    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
    AWS_LS_EVENT_STREAM_RPC_SERVER,
    AWS_LS_EVENT_STREAM_RPC_CLIENT,

    AWS_LS_EVENT_STREAM_LAST = AWS_LOG_SUBJECT_END_RANGE(AWS_C_EVENT_STREAM_PACKAGE_ID),
};

struct aws_event_stream_message_prelude {
    uint32_t total_len;
    uint32_t headers_len;
    uint32_t prelude_crc;
};

struct aws_event_stream_message {
    struct aws_allocator *alloc;
    struct aws_byte_buf message_buffer;
};

/* Prelude (12-bytes) = Total Byte Length (4-bytes) + Headers Byte Length (4-bytes) + Prelude CRC (4-bytes) */
#define AWS_EVENT_STREAM_PRELUDE_LENGTH (uint32_t)(12)

/* Trailer (4-bytes) = Message CRC (4-bytes) */
#define AWS_EVENT_STREAM_TRAILER_LENGTH (uint32_t)(4)

enum aws_event_stream_header_value_type {
    AWS_EVENT_STREAM_HEADER_BOOL_TRUE = 0,
    AWS_EVENT_STREAM_HEADER_BOOL_FALSE,
    AWS_EVENT_STREAM_HEADER_BYTE,
    AWS_EVENT_STREAM_HEADER_INT16,
    AWS_EVENT_STREAM_HEADER_INT32,
    AWS_EVENT_STREAM_HEADER_INT64,
    AWS_EVENT_STREAM_HEADER_BYTE_BUF,
    AWS_EVENT_STREAM_HEADER_STRING,
    /* 64 bit integer (millis since epoch) */
    AWS_EVENT_STREAM_HEADER_TIMESTAMP,
    AWS_EVENT_STREAM_HEADER_UUID
};

struct aws_event_stream_header_value_pair {
    uint8_t header_name_len;
    char header_name[INT8_MAX];
    enum aws_event_stream_header_value_type header_value_type;
    union {
        uint8_t *variable_len_val;
        uint8_t static_val[AWS_EVENT_STREAM_HEADER_STATIC_VALUE_LEN_MAX];
    } header_value;

    uint16_t header_value_len;
    int8_t value_owned;
};

struct aws_event_stream_streaming_decoder;
typedef int(aws_event_stream_process_state_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    const uint8_t *data,
    size_t len,
    size_t *processed);

/**
 * Called by aws_aws_event_stream_streaming_decoder when payload data has been received.
 * 'data' doesn't belong to you, so copy the data if it is needed beyond the scope of your callback.
 * final_segment == 1 indicates the current data is the last payload buffer for that message.
 */
typedef void(aws_event_stream_process_on_payload_segment_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_byte_buf *payload,
    int8_t final_segment,
    void *user_data);

/**
 * Called by aws_aws_event_stream_streaming_decoder when a new message has arrived. The prelude will contain metadata
 * about the message. At this point no headers or payload have been received. prelude is copyable.
 */
typedef void(aws_event_stream_prelude_received_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_event_stream_message_prelude *prelude,
    void *user_data);

/**
 * Called by aws_aws_event_stream_streaming_decoder when a header is encountered. 'header' is not yours. Copy the data
 * you want from it if your scope extends beyond your callback.
 */
typedef void(aws_event_stream_header_received_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_event_stream_message_prelude *prelude,
    struct aws_event_stream_header_value_pair *header,
    void *user_data);

/**
 * Called by aws_aws_event_stream_streaming_decoder when a message decoding is complete
 * and crc is verified.
 */
typedef void(aws_event_stream_on_complete_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    uint32_t message_crc,
    void *user_data);

/**
 * Called by aws_aws_event_stream_streaming_decoder when an error is encountered. The decoder is not in a good state for
 * usage after this callback.
 */
typedef void(aws_event_stream_on_error_fn)(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_event_stream_message_prelude *prelude,
    int error_code,
    const char *message,
    void *user_data);

struct aws_event_stream_streaming_decoder {
    struct aws_allocator *alloc;
    uint8_t working_buffer[AWS_EVENT_STREAM_PRELUDE_LENGTH];
    size_t message_pos;
    uint32_t running_crc;
    size_t current_header_name_offset;
    size_t current_header_value_offset;
    struct aws_event_stream_header_value_pair current_header;
    struct aws_event_stream_message_prelude prelude;
    aws_event_stream_process_state_fn *state;
    aws_event_stream_process_on_payload_segment_fn *on_payload;
    aws_event_stream_prelude_received_fn *on_prelude;
    aws_event_stream_header_received_fn *on_header;
    aws_event_stream_on_complete_fn *on_complete;
    aws_event_stream_on_error_fn *on_error;
    void *user_context;
};

struct aws_event_stream_streaming_decoder_options {
    /**
     * (Required)
     * Invoked repeatedly as payload segment are received.
     * See `aws_event_stream_process_on_payload_segment_fn`.
     */
    aws_event_stream_process_on_payload_segment_fn *on_payload_segment;
    /**
     * (Required)
     * Invoked when when a new message has arrived. The prelude will contain metadata about the message.
     * See `aws_event_stream_prelude_received_fn`.
     */
    aws_event_stream_prelude_received_fn *on_prelude;
    /**
     * (Required)
     * Invoked repeatedly as headers are received.
     * See `aws_event_stream_header_received_fn`.
     */
    aws_event_stream_header_received_fn *on_header;
    /**
     * (Optional)
     * Invoked if a message is decoded successfully.
     * See `aws_event_stream_on_complete_fn`.
     */
    aws_event_stream_on_complete_fn *on_complete;
    /**
     * (Required)
     * Invoked when an error is encountered. The decoder is not in a good state for usage after this callback.
     * See `aws_event_stream_on_error_fn`.
     */
    aws_event_stream_on_error_fn *on_error;
    /**
     * (Optional)
     * user_data passed to callbacks.
     */
    void *user_data;
};
AWS_EXTERN_C_BEGIN

/**
 * Initializes with a list of headers, the payload, and a payload length. CRCs will be computed for you.
 * If headers or payload is NULL, then the fields will be appropriately set to indicate no headers and/or no payload.
 * Both payload and headers will result in an allocation.
 */
AWS_EVENT_STREAM_API int aws_event_stream_message_init(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    const struct aws_array_list *headers,
    const struct aws_byte_buf *payload);

/**
 * Zero allocation, Zero copy. The message will simply wrap the buffer. The message functions are only useful as long as
 * buffer is referencable memory.
 */
AWS_EVENT_STREAM_API int aws_event_stream_message_from_buffer(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    struct aws_byte_buf *buffer);

/**
 * Allocates memory and copies buffer. Otherwise the same as aws_aws_event_stream_message_from_buffer. This is slower,
 * but possibly safer.
 */
AWS_EVENT_STREAM_API int aws_event_stream_message_from_buffer_copy(
    struct aws_event_stream_message *message,
    struct aws_allocator *alloc,
    const struct aws_byte_buf *buffer);

/**
 * Cleans up any internally allocated memory. Always call this for API compatibility reasons, even if you only used the
 * aws_aws_event_stream_message_from_buffer function.
 */
AWS_EVENT_STREAM_API void aws_event_stream_message_clean_up(struct aws_event_stream_message *message);

/**
 * Returns the total length of the message (including the length field).
 */
AWS_EVENT_STREAM_API uint32_t aws_event_stream_message_total_length(const struct aws_event_stream_message *message);

/**
 * Returns the length of the headers portion of the message.
 */
AWS_EVENT_STREAM_API uint32_t aws_event_stream_message_headers_len(const struct aws_event_stream_message *message);

/**
 * Returns the prelude crc (crc32)
 */
AWS_EVENT_STREAM_API uint32_t aws_event_stream_message_prelude_crc(const struct aws_event_stream_message *message);

/**
 * Writes the message to fd in json format. All strings and binary fields are base64 encoded.
 */
AWS_EVENT_STREAM_API int aws_event_stream_message_to_debug_str(
    FILE *fd,
    const struct aws_event_stream_message *message);

/**
 * Adds the headers for the message to list. The memory in each header is owned as part of the message, do not free it
 * or pass its address around.
 */
AWS_EVENT_STREAM_API int aws_event_stream_message_headers(
    const struct aws_event_stream_message *message,
    struct aws_array_list *headers);

/**
 * Returns a pointer to the beginning of the message payload.
 */
AWS_EVENT_STREAM_API const uint8_t *aws_event_stream_message_payload(const struct aws_event_stream_message *message);

/**
 * Returns the length of the message payload.
 */
AWS_EVENT_STREAM_API uint32_t aws_event_stream_message_payload_len(const struct aws_event_stream_message *message);

/**
 * Returns the checksum of the entire message (crc32)
 */
AWS_EVENT_STREAM_API uint32_t aws_event_stream_message_message_crc(const struct aws_event_stream_message *message);

/**
 * Returns the message as a buffer ready for transport.
 */
AWS_EVENT_STREAM_API const uint8_t *aws_event_stream_message_buffer(const struct aws_event_stream_message *message);

AWS_EVENT_STREAM_API uint32_t
    aws_event_stream_compute_headers_required_buffer_len(const struct aws_array_list *headers);

/**
 * Writes headers to buf assuming buf is large enough to hold the data. Prefer this function over the unsafe variant
 * 'aws_event_stream_write_headers_to_buffer'.
 *
 * Returns AWS_OP_SUCCESS if the headers were successfully and completely written and AWS_OP_ERR otherwise.
 */
AWS_EVENT_STREAM_API int aws_event_stream_write_headers_to_buffer_safe(
    const struct aws_array_list *headers,
    struct aws_byte_buf *buf);

/**
 * Deprecated in favor of 'aws_event_stream_write_headers_to_buffer_safe' as this API is unsafe.
 *
 * Writes headers to buffer and returns the length of bytes written to buffer. Assumes buffer is large enough to
 * store the headers.
 */
AWS_EVENT_STREAM_API size_t
    aws_event_stream_write_headers_to_buffer(const struct aws_array_list *headers, uint8_t *buffer);

/** Get the headers from the buffer, store them in the headers list.
 * the user's responsibility to cleanup the list when they are finished with it.
 * no buffer copies happen here, the lifetime of the buffer, must outlive the usage of the headers.
 * returns error codes defined in the public interface.
 */
AWS_EVENT_STREAM_API int aws_event_stream_read_headers_from_buffer(
    struct aws_array_list *headers,
    const uint8_t *buffer,
    size_t headers_len);

/**
 * Initialize a streaming decoder for messages with callbacks for usage
 * and an optional user context pointer.
 */
AWS_EVENT_STREAM_API
void aws_event_stream_streaming_decoder_init_from_options(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_allocator *allocator,
    const struct aws_event_stream_streaming_decoder_options *options);

/**
 * Deprecated. Use aws_event_stream_streaming_decoder_init_from_options instead.
 * Initialize a streaming decoder for messages with callbacks for usage and an optional user context pointer.
 */
AWS_EVENT_STREAM_API void aws_event_stream_streaming_decoder_init(
    struct aws_event_stream_streaming_decoder *decoder,
    struct aws_allocator *alloc,
    aws_event_stream_process_on_payload_segment_fn *on_payload_segment,
    aws_event_stream_prelude_received_fn *on_prelude,
    aws_event_stream_header_received_fn *on_header,
    aws_event_stream_on_error_fn *on_error,
    void *user_data);

/**
 * Currently, no memory is allocated inside aws_aws_event_stream_streaming_decoder, but for future API compatibility,
 * you should call this when finished with it.
 */
AWS_EVENT_STREAM_API void aws_event_stream_streaming_decoder_clean_up(
    struct aws_event_stream_streaming_decoder *decoder);

/**
 * initializes a headers list for you. It will default to a capacity of 4 in dynamic mode.
 */
AWS_EVENT_STREAM_API int aws_event_stream_headers_list_init(
    struct aws_array_list *headers,
    struct aws_allocator *allocator);

/**
 * Cleans up the headers list. Also deallocates any headers that were the result of a copy flag for strings or buffer.
 */
AWS_EVENT_STREAM_API void aws_event_stream_headers_list_cleanup(struct aws_array_list *headers);

/**
 * Adds a string header to the list of headers. If copy is set to true, this will result in an allocation for the header
 * value. Otherwise, the value will be set to the memory address of 'value'.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_string_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    const char *value,
    uint16_t value_len,
    int8_t copy);

AWS_EVENT_STREAM_API struct aws_event_stream_header_value_pair aws_event_stream_create_string_header(
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

AWS_EVENT_STREAM_API struct aws_event_stream_header_value_pair aws_event_stream_create_int32_header(
    struct aws_byte_cursor name,
    int32_t value);

/**
 * Adds a byte header to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_byte_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int8_t value);

/**
 * Adds a bool header to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_bool_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int8_t value);

/**
 * adds a 16 bit integer to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int16_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int16_t value);

/**
 * adds a 32 bit integer to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int32_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int32_t value);

/**
 * adds a 64 bit integer to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int64_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int64_t value);

/**
 * Adds a byte-buffer header to the list of headers. If copy is set to true, this will result in an allocation for the
 * header value. Otherwise, the value will be set to the memory address of 'value'.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_bytebuf_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    uint8_t *value,
    uint16_t value_len,
    int8_t copy);

/**
 * adds a 64 bit integer representing milliseconds since unix epoch to the list of headers.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_timestamp_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    int64_t value);

/**
 * adds a uuid buffer to the list of headers. Value must always be 16 bytes long.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_uuid_header(
    struct aws_array_list *headers,
    const char *name,
    uint8_t name_len,
    const uint8_t *value);

/**
 * Adds a generic header to the list of headers.
 * Makes a copy of the underlaying data.
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_header(
    struct aws_array_list *headers,
    const struct aws_event_stream_header_value_pair *header);

/* Cursor-based header APIs */

/**
 * Adds a boolean-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_bool_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    bool value);

/**
 * Adds a byte-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_byte_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int8_t value);

/**
 * Adds a int16-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int16_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int16_t value);

/**
 * Adds a int32-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int32_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int32_t value);

/**
 * Adds a int64-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_int64_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int64_t value);

/**
 * Adds a string-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_string_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

/**
 * Adds a byte_buf-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_byte_buf_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

/**
 * Adds a timestamp-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_timestamp_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    int64_t value);

/**
 * Adds a uuid-valued header to a header list
 *
 * @param headers header list to add to
 * @param name name of the header to add
 * @param value value of the header to add
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_EVENT_STREAM_API int aws_event_stream_add_uuid_header_by_cursor(
    struct aws_array_list *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

/**
 * Returns the header name. Note: this value is not null terminated
 */
AWS_EVENT_STREAM_API struct aws_byte_buf aws_event_stream_header_name(
    struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a string. Note: this value is not null terminated.
 */
AWS_EVENT_STREAM_API struct aws_byte_buf aws_event_stream_header_value_as_string(
    struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a byte
 */
AWS_EVENT_STREAM_API int8_t aws_event_stream_header_value_as_byte(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a boolean value.
 */
AWS_EVENT_STREAM_API int8_t aws_event_stream_header_value_as_bool(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a 16 bit integer.
 */
AWS_EVENT_STREAM_API int16_t aws_event_stream_header_value_as_int16(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a 32 bit integer.
 */
AWS_EVENT_STREAM_API int32_t aws_event_stream_header_value_as_int32(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a 64 bit integer.
 */
AWS_EVENT_STREAM_API int64_t aws_event_stream_header_value_as_int64(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a pointer to a byte buffer, call aws_event_stream_header_value_length to determine
 * the length of the buffer.
 */
AWS_EVENT_STREAM_API struct aws_byte_buf aws_event_stream_header_value_as_bytebuf(
    struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value as a 64 bit integer representing milliseconds since unix epoch.
 */
AWS_EVENT_STREAM_API int64_t
    aws_event_stream_header_value_as_timestamp(struct aws_event_stream_header_value_pair *header);

/**
 * Returns the header value a byte buffer which is 16 bytes long. Represents a UUID.
 */
AWS_EVENT_STREAM_API struct aws_byte_buf aws_event_stream_header_value_as_uuid(
    struct aws_event_stream_header_value_pair *header);

/**
 * Returns the length of the header value buffer. This is mostly intended for string and byte buffer types.
 */
AWS_EVENT_STREAM_API uint16_t aws_event_stream_header_value_length(struct aws_event_stream_header_value_pair *header);

/**
 * The main driver of the decoder. Pass data that should be decoded with its length. A likely use-case here is in
 * response to a read event from an io-device
 */
AWS_EVENT_STREAM_API int aws_event_stream_streaming_decoder_pump(
    struct aws_event_stream_streaming_decoder *decoder,
    const struct aws_byte_buf *data);

/**
 * Initializes internal datastructures used by aws-c-event-stream.
 * Must be called before using any functionality in aws-c-event-stream.
 */
AWS_EVENT_STREAM_API void aws_event_stream_library_init(struct aws_allocator *allocator);

/**
 * Clean up internal datastructures used by aws-c-event-stream.
 * Must not be called until application is done using functionality in aws-c-event-stream.
 */
AWS_EVENT_STREAM_API void aws_event_stream_library_clean_up(void);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_EVENT_STREAM_H_ */
