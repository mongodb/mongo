#ifndef AWS_MQTT_CLIENT_TEST_HELPER_H
#define AWS_MQTT_CLIENT_TEST_HELPER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/stdint.h>
#include <aws/mqtt/exports.h>

struct aws_allocator;
struct aws_byte_cursor;
struct aws_mqtt_client_connection_311_impl;
struct aws_string;

AWS_EXTERN_C_BEGIN

/** This is for testing applications sending MQTT payloads. Don't ever include this file outside of a unit test. */

/** result buffer will be initialized and payload will be written into it */
AWS_MQTT_API
int aws_mqtt_client_get_payload_for_outstanding_publish_packet(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    struct aws_allocator *allocator,
    struct aws_byte_buf *result);

AWS_MQTT_API
int aws_mqtt_client_get_topic_for_outstanding_publish_packet(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    struct aws_allocator *allocator,
    struct aws_string **result);

AWS_EXTERN_C_END

#endif // AWS_C_IOT_MQTT_CLIENT_TEST_HELPER_H
