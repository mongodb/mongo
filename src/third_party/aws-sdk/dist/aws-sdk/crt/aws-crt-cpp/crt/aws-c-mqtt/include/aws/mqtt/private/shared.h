/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_SHARED_CONSTANTS_H
#define AWS_MQTT_SHARED_CONSTANTS_H

#include <aws/mqtt/mqtt.h>

#include <aws/mqtt/private/request-response/subscription_manager.h>

AWS_EXTERN_C_BEGIN

AWS_MQTT_API extern const struct aws_byte_cursor *g_websocket_handshake_default_path;
AWS_MQTT_API extern const struct aws_http_header *g_websocket_handshake_default_protocol_header;

AWS_EXTERN_C_END

#endif /* AWS_MQTT_SHARED_CONSTANTS_H */
