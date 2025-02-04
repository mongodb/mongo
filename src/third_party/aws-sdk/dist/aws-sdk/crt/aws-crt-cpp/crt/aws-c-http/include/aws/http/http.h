#ifndef AWS_HTTP_H
#define AWS_HTTP_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/logging.h>
#include <aws/http/exports.h>
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_C_HTTP_PACKAGE_ID 2

enum aws_http_errors {
    AWS_ERROR_HTTP_UNKNOWN = AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_HTTP_PACKAGE_ID),
    AWS_ERROR_HTTP_HEADER_NOT_FOUND,
    AWS_ERROR_HTTP_INVALID_HEADER_FIELD,
    AWS_ERROR_HTTP_INVALID_HEADER_NAME,
    AWS_ERROR_HTTP_INVALID_HEADER_VALUE,
    AWS_ERROR_HTTP_INVALID_METHOD,
    AWS_ERROR_HTTP_INVALID_PATH,
    AWS_ERROR_HTTP_INVALID_STATUS_CODE,
    AWS_ERROR_HTTP_MISSING_BODY_STREAM,
    AWS_ERROR_HTTP_INVALID_BODY_STREAM,
    AWS_ERROR_HTTP_CONNECTION_CLOSED,
    AWS_ERROR_HTTP_SWITCHED_PROTOCOLS,
    AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL,
    AWS_ERROR_HTTP_REACTION_REQUIRED,
    AWS_ERROR_HTTP_DATA_NOT_AVAILABLE,
    AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT,
    AWS_ERROR_HTTP_CALLBACK_FAILURE,
    AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE,
    AWS_ERROR_HTTP_WEBSOCKET_CLOSE_FRAME_SENT,
    AWS_ERROR_HTTP_WEBSOCKET_IS_MIDCHANNEL_HANDLER,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_INVALID_STATE_FOR_ACQUIRE,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_VENDED_CONNECTION_UNDERFLOW,
    AWS_ERROR_HTTP_SERVER_CLOSED,
    AWS_ERROR_HTTP_PROXY_CONNECT_FAILED,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_SHUTTING_DOWN,
    AWS_ERROR_HTTP_CHANNEL_THROUGHPUT_FAILURE,
    AWS_ERROR_HTTP_PROTOCOL_ERROR,
    AWS_ERROR_HTTP_STREAM_IDS_EXHAUSTED,
    AWS_ERROR_HTTP_GOAWAY_RECEIVED,
    AWS_ERROR_HTTP_RST_STREAM_RECEIVED,
    AWS_ERROR_HTTP_RST_STREAM_SENT,
    AWS_ERROR_HTTP_STREAM_NOT_ACTIVATED,
    AWS_ERROR_HTTP_STREAM_HAS_COMPLETED,
    AWS_ERROR_HTTP_PROXY_STRATEGY_NTLM_CHALLENGE_TOKEN_MISSING,
    AWS_ERROR_HTTP_PROXY_STRATEGY_TOKEN_RETRIEVAL_FAILURE,
    AWS_ERROR_HTTP_PROXY_CONNECT_FAILED_RETRYABLE,
    AWS_ERROR_HTTP_PROTOCOL_SWITCH_FAILURE,
    AWS_ERROR_HTTP_MAX_CONCURRENT_STREAMS_EXCEEDED,
    AWS_ERROR_HTTP_STREAM_MANAGER_SHUTTING_DOWN,
    AWS_ERROR_HTTP_STREAM_MANAGER_CONNECTION_ACQUIRE_FAILURE,
    AWS_ERROR_HTTP_STREAM_MANAGER_UNEXPECTED_HTTP_VERSION,
    AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR,
    AWS_ERROR_HTTP_MANUAL_WRITE_NOT_ENABLED,
    AWS_ERROR_HTTP_MANUAL_WRITE_HAS_COMPLETED,
    AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_ACQUISITION_TIMEOUT,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_MAX_PENDING_ACQUISITIONS_EXCEEDED,

    AWS_ERROR_HTTP_END_RANGE = AWS_ERROR_ENUM_END_RANGE(AWS_C_HTTP_PACKAGE_ID)
};

/* Error codes that may be present in HTTP/2 RST_STREAM and GOAWAY frames (RFC-7540 7). */
enum aws_http2_error_code {
    AWS_HTTP2_ERR_NO_ERROR = 0x00,
    AWS_HTTP2_ERR_PROTOCOL_ERROR = 0x01,
    AWS_HTTP2_ERR_INTERNAL_ERROR = 0x02,
    AWS_HTTP2_ERR_FLOW_CONTROL_ERROR = 0x03,
    AWS_HTTP2_ERR_SETTINGS_TIMEOUT = 0x04,
    AWS_HTTP2_ERR_STREAM_CLOSED = 0x05,
    AWS_HTTP2_ERR_FRAME_SIZE_ERROR = 0x06,
    AWS_HTTP2_ERR_REFUSED_STREAM = 0x07,
    AWS_HTTP2_ERR_CANCEL = 0x08,
    AWS_HTTP2_ERR_COMPRESSION_ERROR = 0x09,
    AWS_HTTP2_ERR_CONNECT_ERROR = 0x0A,
    AWS_HTTP2_ERR_ENHANCE_YOUR_CALM = 0x0B,
    AWS_HTTP2_ERR_INADEQUATE_SECURITY = 0x0C,
    AWS_HTTP2_ERR_HTTP_1_1_REQUIRED = 0x0D,
    AWS_HTTP2_ERR_COUNT,
};

enum aws_http_log_subject {
    AWS_LS_HTTP_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_HTTP_PACKAGE_ID),
    AWS_LS_HTTP_CONNECTION,
    AWS_LS_HTTP_ENCODER,
    AWS_LS_HTTP_DECODER,
    AWS_LS_HTTP_SERVER,
    AWS_LS_HTTP_STREAM,
    AWS_LS_HTTP_CONNECTION_MANAGER,
    AWS_LS_HTTP_STREAM_MANAGER,
    AWS_LS_HTTP_WEBSOCKET,
    AWS_LS_HTTP_WEBSOCKET_SETUP,
    AWS_LS_HTTP_PROXY_NEGOTIATION,
};

enum aws_http_version {
    AWS_HTTP_VERSION_UNKNOWN, /* Invalid version. */
    AWS_HTTP_VERSION_1_0,
    AWS_HTTP_VERSION_1_1,
    AWS_HTTP_VERSION_2,
    AWS_HTTP_VERSION_COUNT,
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes internal datastructures used by aws-c-http.
 * Must be called before using any functionality in aws-c-http.
 */
AWS_HTTP_API
void aws_http_library_init(struct aws_allocator *alloc);

/**
 * Clean up internal datastructures used by aws-c-http.
 * Must not be called until application is done using functionality in aws-c-http.
 */
AWS_HTTP_API
void aws_http_library_clean_up(void);

/**
 * Returns the description of common status codes.
 * Ex: 404 -> "Not Found"
 * An empty string is returned if the status code is not recognized.
 */
AWS_HTTP_API
const char *aws_http_status_text(int status_code);

/**
 * Shortcuts for common HTTP request methods
 */
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_get;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_head;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_post;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_put;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_delete;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_connect;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_options;

AWS_HTTP_API extern const struct aws_byte_cursor aws_http_header_method;
AWS_HTTP_API extern const struct aws_byte_cursor aws_http_header_scheme;
AWS_HTTP_API extern const struct aws_byte_cursor aws_http_header_authority;
AWS_HTTP_API extern const struct aws_byte_cursor aws_http_header_path;
AWS_HTTP_API extern const struct aws_byte_cursor aws_http_header_status;

AWS_HTTP_API extern const struct aws_byte_cursor aws_http_scheme_http;
AWS_HTTP_API extern const struct aws_byte_cursor aws_http_scheme_https;

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_H */
