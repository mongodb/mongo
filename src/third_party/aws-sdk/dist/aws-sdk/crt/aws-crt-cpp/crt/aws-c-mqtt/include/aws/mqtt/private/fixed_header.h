#ifndef AWS_MQTT_PRIVATE_FIXED_HEADER_H
#define AWS_MQTT_PRIVATE_FIXED_HEADER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>

#include <aws/mqtt/mqtt.h>

/* Represents the types of the MQTT control packets [MQTT-2.2.1]. */
enum aws_mqtt_packet_type {
    /* reserved = 0, */
    AWS_MQTT_PACKET_CONNECT = 1,
    AWS_MQTT_PACKET_CONNACK,
    AWS_MQTT_PACKET_PUBLISH,
    AWS_MQTT_PACKET_PUBACK,
    AWS_MQTT_PACKET_PUBREC,
    AWS_MQTT_PACKET_PUBREL,
    AWS_MQTT_PACKET_PUBCOMP,
    AWS_MQTT_PACKET_SUBSCRIBE,
    AWS_MQTT_PACKET_SUBACK,
    AWS_MQTT_PACKET_UNSUBSCRIBE,
    AWS_MQTT_PACKET_UNSUBACK,
    AWS_MQTT_PACKET_PINGREQ,
    AWS_MQTT_PACKET_PINGRESP,
    AWS_MQTT_PACKET_DISCONNECT,
    /* reserved = 15, */
};

/**
 * Represents the fixed header [MQTT-2.2].
 */
struct aws_mqtt_fixed_header {
    enum aws_mqtt_packet_type packet_type;
    size_t remaining_length;
    uint8_t flags;
};

/**
 * Get the type of packet from the first byte of the buffer [MQTT-2.2.1].
 */
AWS_MQTT_API enum aws_mqtt_packet_type aws_mqtt_get_packet_type(const uint8_t *buffer);

/**
 * Get traits describing a packet described by header [MQTT-2.2.2].
 */
AWS_MQTT_API bool aws_mqtt_packet_has_flags(const struct aws_mqtt_fixed_header *header);

/**
 * Write a fixed header to a byte stream.
 */
AWS_MQTT_API int aws_mqtt_fixed_header_encode(struct aws_byte_buf *buf, const struct aws_mqtt_fixed_header *header);

/**
 * Read a fixed header from a byte stream.
 */
AWS_MQTT_API int aws_mqtt_fixed_header_decode(struct aws_byte_cursor *cur, struct aws_mqtt_fixed_header *header);

AWS_MQTT_API int aws_mqtt311_decode_remaining_length(struct aws_byte_cursor *cur, size_t *remaining_length_out);

#endif /* AWS_MQTT_PRIVATE_FIXED_HEADER_H */
