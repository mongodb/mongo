#ifndef AWS_MQTT_PRIVATE_PACKETS_H
#define AWS_MQTT_PRIVATE_PACKETS_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>
#include <aws/mqtt/private/fixed_header.h>

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>
#include <aws/common/string.h>

/*
 * General MQTT Control Packet Format [MQTT-2]:
 *     1. Fixed header, present in all packets
 *     2. Variable header, present in some packets
 *     3. Payload, preset in some packets
 */

/* Struct used internally for representing subscriptions */
struct aws_mqtt_subscription {
    /* Topic filter to subscribe to [MQTT-4.7]. */
    struct aws_byte_cursor topic_filter;
    /* Maximum QoS of messages to receive [MQTT-4.3]. */
    enum aws_mqtt_qos qos;
};

/**
 * Used to represent the following MQTT packets:
 * - PUBACK
 * - PUBREC
 * - PUBREL
 * - PUBCOMP
 * - UNSUBACK
 */
struct aws_mqtt_packet_ack {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    uint16_t packet_identifier;
};

/**
 * Represents the MQTT SUBACK packet
 */
struct aws_mqtt_packet_suback {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    uint16_t packet_identifier;

    /* Payload */
    /* List of uint8_t return code */
    struct aws_array_list return_codes;
};

/* Represents the MQTT CONNECT packet */
struct aws_mqtt_packet_connect {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    bool clean_session;
    bool has_will;
    bool will_retain;
    bool has_password;
    bool has_username;
    uint16_t keep_alive_timeout;
    enum aws_mqtt_qos will_qos;
    struct aws_byte_cursor client_identifier;

    /* Payload */
    struct aws_byte_cursor will_topic;
    struct aws_byte_cursor will_message;
    struct aws_byte_cursor username;
    struct aws_byte_cursor password;
};

/* Represents the MQTT CONNACK packet */
struct aws_mqtt_packet_connack {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    bool session_present;
    uint8_t connect_return_code;
};

/* Represents the MQTT PUBLISH packet */
struct aws_mqtt_packet_publish {
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    uint16_t packet_identifier;
    struct aws_byte_cursor topic_name;

    /* Payload */
    struct aws_byte_cursor payload;
};

/* Represents the MQTT SUBSCRIBE packet */
struct aws_mqtt_packet_subscribe {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    uint16_t packet_identifier;

    /* Payload */
    /* List of aws_mqtt_subscription */
    struct aws_array_list topic_filters;
};

/* Represents the MQTT UNSUBSCRIBE packet */
struct aws_mqtt_packet_unsubscribe {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;

    /* Variable header */
    uint16_t packet_identifier;

    /* Payload */
    /* List of aws_byte_cursors */
    struct aws_array_list topic_filters;
};
/**
 * Used to represent the following MQTT packets:
 * - PINGREQ
 * - PINGRESP
 * - DISCONNECT
 */
struct aws_mqtt_packet_connection {
    /* Fixed header */
    struct aws_mqtt_fixed_header fixed_header;
};

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************/
/* Ack                                                                       */

AWS_MQTT_API
int aws_mqtt_packet_ack_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_ack *packet);

AWS_MQTT_API
int aws_mqtt_packet_ack_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_ack *packet);

/*****************************************************************************/
/* Connect                                                                   */

AWS_MQTT_API
int aws_mqtt_packet_connect_init(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor client_identifier,
    bool clean_session,
    uint16_t keep_alive);

AWS_MQTT_API
int aws_mqtt_packet_connect_add_will(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor topic,
    enum aws_mqtt_qos qos,
    bool retain,
    struct aws_byte_cursor payload);

AWS_MQTT_API
int aws_mqtt_packet_connect_add_credentials(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor username,
    struct aws_byte_cursor password);

AWS_MQTT_API
int aws_mqtt_packet_connect_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connect *packet);

AWS_MQTT_API
int aws_mqtt_packet_connect_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connect *packet);

/*****************************************************************************/
/* Connack                                                                   */

AWS_MQTT_API
int aws_mqtt_packet_connack_init(
    struct aws_mqtt_packet_connack *packet,
    bool session_present,
    enum aws_mqtt_connect_return_code return_code);

AWS_MQTT_API
int aws_mqtt_packet_connack_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connack *packet);

AWS_MQTT_API
int aws_mqtt_packet_connack_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connack *packet);

/*****************************************************************************/
/* Publish                                                                   */

AWS_MQTT_API
int aws_mqtt_packet_publish_init(
    struct aws_mqtt_packet_publish *packet,
    bool retain,
    enum aws_mqtt_qos qos,
    bool dup,
    struct aws_byte_cursor topic_name,
    uint16_t packet_identifier,
    struct aws_byte_cursor payload);

AWS_MQTT_API
int aws_mqtt_packet_publish_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
int aws_mqtt_packet_publish_encode_headers(struct aws_byte_buf *buf, const struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
int aws_mqtt_packet_publish_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
void aws_mqtt_packet_publish_set_dup(struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
bool aws_mqtt_packet_publish_get_dup(const struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
enum aws_mqtt_qos aws_mqtt_packet_publish_get_qos(const struct aws_mqtt_packet_publish *packet);

AWS_MQTT_API
bool aws_mqtt_packet_publish_get_retain(const struct aws_mqtt_packet_publish *packet);

/*****************************************************************************/
/* Puback                                                                    */

AWS_MQTT_API
int aws_mqtt_packet_puback_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier);

/*****************************************************************************/
/* Pubrec                                                                    */

AWS_MQTT_API
int aws_mqtt_packet_pubrec_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier);

/*****************************************************************************/
/* Pubrel                                                                    */

AWS_MQTT_API
int aws_mqtt_packet_pubrel_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier);

/*****************************************************************************/
/* Pubcomp                                                                   */

AWS_MQTT_API
int aws_mqtt_packet_pubcomp_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier);

/*****************************************************************************/
/* Subscribe                                                                 */

AWS_MQTT_API
int aws_mqtt_packet_subscribe_init(
    struct aws_mqtt_packet_subscribe *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier);

AWS_MQTT_API
void aws_mqtt_packet_subscribe_clean_up(struct aws_mqtt_packet_subscribe *packet);

AWS_MQTT_API
int aws_mqtt_packet_subscribe_add_topic(
    struct aws_mqtt_packet_subscribe *packet,
    struct aws_byte_cursor topic_filter,
    enum aws_mqtt_qos qos);

AWS_MQTT_API
int aws_mqtt_packet_subscribe_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_subscribe *packet);

AWS_MQTT_API
int aws_mqtt_packet_subscribe_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_subscribe *packet);

/*****************************************************************************/
/* Suback                                                                    */

AWS_MQTT_API
int aws_mqtt_packet_suback_init(
    struct aws_mqtt_packet_suback *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier);

AWS_MQTT_API
void aws_mqtt_packet_suback_clean_up(struct aws_mqtt_packet_suback *packet);

AWS_MQTT_API
int aws_mqtt_packet_suback_add_return_code(struct aws_mqtt_packet_suback *packet, uint8_t return_code);

AWS_MQTT_API
int aws_mqtt_packet_suback_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_suback *packet);

AWS_MQTT_API
int aws_mqtt_packet_suback_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_suback *packet);

/*****************************************************************************/
/* Unsubscribe                                                               */

AWS_MQTT_API
int aws_mqtt_packet_unsubscribe_init(
    struct aws_mqtt_packet_unsubscribe *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier);

AWS_MQTT_API
void aws_mqtt_packet_unsubscribe_clean_up(struct aws_mqtt_packet_unsubscribe *packet);

AWS_MQTT_API
int aws_mqtt_packet_unsubscribe_add_topic(
    struct aws_mqtt_packet_unsubscribe *packet,
    struct aws_byte_cursor topic_filter);

AWS_MQTT_API
int aws_mqtt_packet_unsubscribe_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_unsubscribe *packet);

AWS_MQTT_API
int aws_mqtt_packet_unsubscribe_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_unsubscribe *packet);

/*****************************************************************************/
/* Unsuback                                                                  */

AWS_MQTT_API
int aws_mqtt_packet_unsuback_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier);

/*****************************************************************************/
/* Ping request/response, disconnect                                         */

AWS_MQTT_API
int aws_mqtt_packet_pingreq_init(struct aws_mqtt_packet_connection *packet);

AWS_MQTT_API
int aws_mqtt_packet_pingresp_init(struct aws_mqtt_packet_connection *packet);

AWS_MQTT_API
int aws_mqtt_packet_disconnect_init(struct aws_mqtt_packet_connection *packet);

AWS_MQTT_API
int aws_mqtt_packet_connection_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connection *packet);

AWS_MQTT_API
int aws_mqtt_packet_connection_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connection *packet);

#ifdef __cplusplus
}
#endif

#endif /* AWS_MQTT_PRIVATE_PACKETS_H */
