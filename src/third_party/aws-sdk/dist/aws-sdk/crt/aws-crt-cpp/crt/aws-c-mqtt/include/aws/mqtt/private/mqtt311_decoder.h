#ifndef AWS_MQTT_PRIVATE_MQTT311_DECODER_H
#define AWS_MQTT_PRIVATE_MQTT311_DECODER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/common/byte_buf.h>

/*
 * Per-packet-type callback signature.  message_cursor contains the entire packet's data.
 */
typedef int(packet_handler_fn)(struct aws_byte_cursor message_cursor, void *user_data);

/*
 * Wrapper for a set of packet handlers for each possible MQTT packet type.  Some values are invalid in 311 (15), and
 * some values are only valid from the perspective of the server or client.
 */
struct aws_mqtt_client_connection_packet_handlers {
    packet_handler_fn *handlers_by_packet_type[16];
};

/*
 * Internal state of the 311 decoder/framing logic.
 *
 * When a packet is fragmented across multiple io buffers, state moves circularly:
 *      first byte -> remaining length -> body -> first byte etc...
 *
 * When a packet is completely contained inside a single io buffer, the entire packet is processed within
 * the READ_FIRST_BYTE state.
 */
enum aws_mqtt_311_decoder_state_type {

    /*
     * The decoder is expecting the first byte of the fixed header of an MQTT control packet
     */
    AWS_MDST_READ_FIRST_BYTE,

    /*
     * The decoder is expecting the vli-encoded total remaining length field of the fixed header on an MQTT control
     * packet.
     */
    AWS_MDST_READ_REMAINING_LENGTH,

    /*
     * The decoder is expecting the "rest" of the MQTT packet's data based on the remaining length value that has
     * already been read.
     */
    AWS_MDST_READ_BODY,

    /*
     * Terminal state for when a protocol error has been encountered by the decoder.  The only way to leave this
     * state is to reset the decoder via the aws_mqtt311_decoder_reset_for_new_connection() API.
     */
    AWS_MDST_PROTOCOL_ERROR,
};

/*
 * Configuration options for the decoder.  When used by the actual implementation, handler_user_data is the
 * connection object and the packet handlers are channel functions that hook into reactionary behavior and user
 * callbacks.
 */
struct aws_mqtt311_decoder_options {
    const struct aws_mqtt_client_connection_packet_handlers *packet_handlers;
    void *handler_user_data;
};

/*
 * Simple MQTT311 decoder structure.  Actual decoding is deferred to per-packet functions that expect the whole
 * packet in a buffer.  The primary function of this sub-system is correctly framing a stream of bytes into the
 * constituent packets.
 */
struct aws_mqtt311_decoder {
    struct aws_mqtt311_decoder_options config;

    enum aws_mqtt_311_decoder_state_type state;

    /*
     * If zero, not valid.  If non-zero, represents the number of bytes that need to be read to finish the packet.
     * This includes the total encoding size of the fixed header.
     */
    size_t total_packet_length;

    /* scratch buffer to hold individual packets when they fragment across incoming data frame boundaries */
    struct aws_byte_buf packet_buffer;
};

AWS_EXTERN_C_BEGIN

/**
 * Initialize function for the MQTT311 decoder
 *
 * @param decoder decoder to initialize
 * @param allocator memory allocator to use
 * @param options additional decoder configuration options
 */
AWS_MQTT_API void aws_mqtt311_decoder_init(
    struct aws_mqtt311_decoder *decoder,
    struct aws_allocator *allocator,
    const struct aws_mqtt311_decoder_options *options);

/**
 * Clean up function for an MQTT311 decoder
 *
 * @param decoder decoder to release resources for
 */
AWS_MQTT_API void aws_mqtt311_decoder_clean_up(struct aws_mqtt311_decoder *decoder);

/**
 * Callback function to decode the incoming data stream of an MQTT311 connection.  Handles packet framing and
 * correct decoder/handler function dispatch.
 *
 * @param decoder decoder to decode with
 * @param data raw plaintext bytes of a connection operating on the MQTT311 protocol
 * @return success/failure, failure represents a protocol error and implies the connection must be shut down
 */
AWS_MQTT_API int aws_mqtt311_decoder_on_bytes_received(
    struct aws_mqtt311_decoder *decoder,
    struct aws_byte_cursor data);

/**
 * Resets a decoder's state to its initial values.  If using a decoder across multiple network connections (within
 * the same client), you must invoke this when setting up a new connection, before any MQTT protocol bytes are
 * processed.  Breaks the decoder out of any previous protocol error terminal state.
 *
 * @param decoder decoder to reset
 */
AWS_MQTT_API void aws_mqtt311_decoder_reset_for_new_connection(struct aws_mqtt311_decoder *decoder);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_PRIVATE_MQTT311_DECODER_H */
