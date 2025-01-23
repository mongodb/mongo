/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/shared.h>

#include <aws/http/request_response.h>

/*
 * These defaults were chosen because they're commmon in other MQTT libraries.
 * The user can modify the request in their transform callback if they need to.
 */
static const struct aws_byte_cursor s_websocket_handshake_default_path = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/mqtt");
const struct aws_byte_cursor *g_websocket_handshake_default_path = &s_websocket_handshake_default_path;

static const struct aws_http_header s_websocket_handshake_default_protocol_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Sec-WebSocket-Protocol"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("mqtt"),
};
const struct aws_http_header *g_websocket_handshake_default_protocol_header =
    &s_websocket_handshake_default_protocol_header;
