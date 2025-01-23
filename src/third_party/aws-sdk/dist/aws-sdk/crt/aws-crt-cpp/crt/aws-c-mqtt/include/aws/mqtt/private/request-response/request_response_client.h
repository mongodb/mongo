#ifndef AWS_MQTT_PRIVATE_REQUEST_RESPONSE_REQUEST_RESPONSE_CLIENT_H
#define AWS_MQTT_PRIVATE_REQUEST_RESPONSE_REQUEST_RESPONSE_CLIENT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/mqtt/request-response/request_response_client.h>

AWS_EXTERN_C_BEGIN

struct aws_mqtt_request_response_client *aws_mqtt_request_response_client_acquire_internal(
    struct aws_mqtt_request_response_client *client);

struct aws_mqtt_request_response_client *aws_mqtt_request_response_client_release_internal(
    struct aws_mqtt_request_response_client *client);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_PRIVATE_REQUEST_RESPONSE_REQUEST_RESPONSE_CLIENT_H */