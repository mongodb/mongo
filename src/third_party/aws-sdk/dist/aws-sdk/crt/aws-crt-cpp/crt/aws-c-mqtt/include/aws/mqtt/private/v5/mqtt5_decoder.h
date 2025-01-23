/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_MQTT5_DECODER_H
#define AWS_MQTT_MQTT5_DECODER_H

#include <aws/mqtt/mqtt.h>

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/mqtt/private/v5/mqtt5_options_storage.h>
#include <aws/mqtt/v5/mqtt5_types.h>

struct aws_mqtt5_client;
struct aws_mqtt5_decoder;
struct aws_mqtt5_inbound_topic_alias_resolver;

/**
 * Overall decoder state.  We read the packet type and the remaining length, and then buffer the
 * entire packet before decoding.
 */
enum aws_mqtt5_decoder_state {
    AWS_MQTT5_DS_READ_PACKET_TYPE,
    AWS_MQTT5_DS_READ_REMAINING_LENGTH,
    AWS_MQTT5_DS_READ_PACKET,
    AWS_MQTT5_DS_FATAL_ERROR,
};

/*
 * Basic return value for a number of different decoding operations.  Error is always fatal and implies the
 * connection needs to be torn down.
 */
enum aws_mqtt5_decode_result_type {
    AWS_MQTT5_DRT_MORE_DATA,
    AWS_MQTT5_DRT_SUCCESS,
    AWS_MQTT5_DRT_ERROR,
};

/*
 * Callbacks the decoder should invoke.  We don't invoke functions directly on the client because
 * we want to test the decoder's correctness in isolation.
 */
typedef int(aws_mqtt5_on_packet_received_fn)(
    enum aws_mqtt5_packet_type type,
    void *packet_view,
    void *decoder_callback_user_data);

typedef int(aws_mqtt5_on_publish_payload_data_fn)(
    struct aws_mqtt5_packet_publish_view *publish_view,
    struct aws_byte_cursor payload,
    void *decoder_callback_user_data);

/**
 * per-packet-type decoding function signature
 */
typedef int(aws_mqtt5_decoding_fn)(struct aws_mqtt5_decoder *decoder);

/**
 * table of decoding functions.  Tests use an augmented version that includes decoders for packet types normally
 * only decoded by an mqtt server.
 */
struct aws_mqtt5_decoder_function_table {
    aws_mqtt5_decoding_fn *decoders_by_packet_type[16];
};

/**
 * Basic decoder configuration.
 */
struct aws_mqtt5_decoder_options {
    void *callback_user_data;
    aws_mqtt5_on_packet_received_fn *on_packet_received;
    const struct aws_mqtt5_decoder_function_table *decoder_table;
};

struct aws_mqtt5_decoder {
    struct aws_allocator *allocator;
    struct aws_mqtt5_decoder_options options;

    enum aws_mqtt5_decoder_state state;

    /*
     * decode scratch space: packets may get fully buffered here before decode
     * Exceptions:
     *   when the incoming io message buffer contains the entire packet, we decode directly from it instead
     */
    struct aws_byte_buf scratch_space;

    /*
     * packet type and flags
     */
    uint8_t packet_first_byte;

    uint32_t remaining_length;

    /*
     * Packet decoders work from this cursor.  It may point to scratch_space (for packets that were delivered
     * in more than one fragment) or to an io message buffer that contains the entire packet.
     */
    struct aws_byte_cursor packet_cursor;

    struct aws_mqtt5_inbound_topic_alias_resolver *topic_alias_resolver;
};

AWS_EXTERN_C_BEGIN

/**
 * One-time initialization for an mqtt5 decoder
 *
 * @param decoder decoder to initialize
 * @param allocator allocator to use for memory allocation
 * @param options configuration options
 * @return success/failure
 */
AWS_MQTT_API int aws_mqtt5_decoder_init(
    struct aws_mqtt5_decoder *decoder,
    struct aws_allocator *allocator,
    struct aws_mqtt5_decoder_options *options);

/**
 * Cleans up an mqtt5 decoder
 *
 * @param decoder decoder to clean up
 */
AWS_MQTT_API void aws_mqtt5_decoder_clean_up(struct aws_mqtt5_decoder *decoder);

/**
 * Resets the state of an mqtt5 decoder.  Used whenever a new connection is established
 *
 * @param decoder decoder to reset state for
 */
AWS_MQTT_API void aws_mqtt5_decoder_reset(struct aws_mqtt5_decoder *decoder);

/**
 * Basic entry point for all incoming mqtt5 data once the basic connection has been established
 *
 * @param decoder decoder to decode data with
 * @param data the data to decode
 * @return success/failure - failure implies a need to shut down the connection
 */
AWS_MQTT_API int aws_mqtt5_decoder_on_data_received(struct aws_mqtt5_decoder *decoder, struct aws_byte_cursor data);

/**
 * Sets the optional inbound alias resolver that the decoder should use during the lifetime of a connection
 *
 * @param decoder decoder to apply inbound topic alias resolution to
 * @param resolver inbound topic alias resolver
 */
AWS_MQTT_API void aws_mqtt5_decoder_set_inbound_topic_alias_resolver(
    struct aws_mqtt5_decoder *decoder,
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver);

/**
 * Default decoding table; tests use an augmented version with decoders for packets that only the server needs to
 * decode.
 */
AWS_MQTT_API extern const struct aws_mqtt5_decoder_function_table *g_aws_mqtt5_default_decoder_table;

AWS_EXTERN_C_END

/* Decode helpers */

AWS_EXTERN_C_BEGIN

/**
 * Decodes, if possible, a variable length integer from a cursor.  If the decode is successful, the cursor is advanced
 * past the variable length integer encoding. This can be used both for streaming and non-streaming decode operations.
 *
 * @param cursor data to decode from
 * @param dest where to put a successfully decoded variable length integer
 * @return the result of attempting the decode: {success, error, not enough data}  Does not set aws_last_error.
 */
AWS_MQTT_API enum aws_mqtt5_decode_result_type aws_mqtt5_decode_vli(struct aws_byte_cursor *cursor, uint32_t *dest);

/**
 * Decodes an MQTT5 user property from a cursor
 *
 * @param packet_cursor data to decode from
 * @param properties property set to add the decoded property to
 * @return success/failure - failures implies connection termination
 */
AWS_MQTT_API int aws_mqtt5_decode_user_property(
    struct aws_byte_cursor *packet_cursor,
    struct aws_mqtt5_user_property_set *properties);

AWS_EXTERN_C_END

/* Decode helper macros operating on a cursor */

/*
 * u8 and u16 decode are a little different in order to support encoded values that are widened to larger storage.
 * To make that safe, we decode to a local and then assign the local to the final spot.  There should be no
 * complaints as long as the implicit conversion is the same size or wider.
 *
 * Some u8 examples include qos (one byte encode -> int-based enum) and various reason codes
 * Some u16 examples include cursor lengths decoded directly into a cursor's len field (u16 -> size_t)
 */
#define AWS_MQTT5_DECODE_U8(cursor_ptr, u8_ptr, error_label)                                                           \
    {                                                                                                                  \
        uint8_t decoded_value = 0;                                                                                     \
        if (!aws_byte_cursor_read_u8((cursor_ptr), (&decoded_value))) {                                                \
            goto error_label;                                                                                          \
        }                                                                                                              \
        *u8_ptr = decoded_value;                                                                                       \
    }

#define AWS_MQTT5_DECODE_U8_OPTIONAL(cursor_ptr, u8_ptr, u8_ptr_ptr, error_label)                                      \
    AWS_MQTT5_DECODE_U8(cursor_ptr, u8_ptr, error_label);                                                              \
    *(u8_ptr_ptr) = (u8_ptr);

#define AWS_MQTT5_DECODE_U16(cursor_ptr, u16_ptr, error_label)                                                         \
    {                                                                                                                  \
        uint16_t decoded_value = 0;                                                                                    \
        if (!aws_byte_cursor_read_be16((cursor_ptr), (&decoded_value))) {                                              \
            goto error_label;                                                                                          \
        }                                                                                                              \
        *u16_ptr = decoded_value;                                                                                      \
    }

/*
 * In addition to decoding a length prefix, this also verifies that the length prefix does not exceed the source
 * cursor length.
 */
#define AWS_MQTT5_DECODE_U16_PREFIX(cursor_ptr, u16_ptr, error_label)                                                  \
    AWS_MQTT5_DECODE_U16((cursor_ptr), (u16_ptr), error_label);                                                        \
    if (cursor_ptr->len < *(u16_ptr)) {                                                                                \
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);                                                        \
        goto error_label;                                                                                              \
    }

#define AWS_MQTT5_DECODE_U16_OPTIONAL(cursor_ptr, u16_ptr, u16_ptr_ptr, error_label)                                   \
    AWS_MQTT5_DECODE_U16((cursor_ptr), u16_ptr, error_label);                                                          \
    *(u16_ptr_ptr) = (u16_ptr);

#define AWS_MQTT5_DECODE_U32(cursor_ptr, u32_ptr, error_label)                                                         \
    if (!aws_byte_cursor_read_be32((cursor_ptr), (u32_ptr))) {                                                         \
        goto error_label;                                                                                              \
    }

#define AWS_MQTT5_DECODE_U32_OPTIONAL(cursor_ptr, u32_ptr, u32_ptr_ptr, error_label)                                   \
    AWS_MQTT5_DECODE_U32((cursor_ptr), u32_ptr, error_label);                                                          \
    *(u32_ptr_ptr) = (u32_ptr);

#define AWS_MQTT5_DECODE_VLI(cursor_ptr, u32_ptr, error_label)                                                         \
    if (AWS_MQTT5_DRT_SUCCESS != aws_mqtt5_decode_vli((cursor_ptr), (u32_ptr))) {                                      \
        goto error_label;                                                                                              \
    }

/* decodes both the length prefix and the following cursor field */
#define AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR(cursor_ptr, dest_cursor_ptr, error_label)                              \
    {                                                                                                                  \
        uint16_t prefix_length = 0;                                                                                    \
        AWS_MQTT5_DECODE_U16_PREFIX((cursor_ptr), &prefix_length, error_label)                                         \
                                                                                                                       \
        *(dest_cursor_ptr) = aws_byte_cursor_advance((cursor_ptr), prefix_length);                                     \
    }

#define AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(                                                              \
    cursor_ptr, dest_cursor_ptr, dest_cursor_ptr_ptr, error_label)                                                     \
    AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR((cursor_ptr), (dest_cursor_ptr), error_label)                              \
    *(dest_cursor_ptr_ptr) = (dest_cursor_ptr);

#endif /* AWS_MQTT_MQTT5_DECODER_H */
