#ifndef AWS_HTTP_STATISTICS_H
#define AWS_HTTP_STATISTICS_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

#include <aws/common/statistics.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_crt_http_statistics_category {
    AWSCRT_STAT_CAT_HTTP1_CHANNEL = AWS_CRT_STATISTICS_CATEGORY_BEGIN_RANGE(AWS_C_HTTP_PACKAGE_ID),
    AWSCRT_STAT_CAT_HTTP2_CHANNEL,
};

/**
 * A statistics struct for http handlers.  Tracks the actual amount of time that incoming and outgoing requests are
 * waiting for their IO to complete.
 */
struct aws_crt_statistics_http1_channel {
    aws_crt_statistics_category_t category;

    uint64_t pending_outgoing_stream_ms;
    uint64_t pending_incoming_stream_ms;

    uint32_t current_outgoing_stream_id;
    uint32_t current_incoming_stream_id;
};

struct aws_crt_statistics_http2_channel {
    aws_crt_statistics_category_t category;

    uint64_t pending_outgoing_stream_ms;
    uint64_t pending_incoming_stream_ms;

    /* True if during the time of report, there has ever been no active streams on the connection */
    bool was_inactive;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes a http channel handler statistics struct
 */
AWS_HTTP_API
int aws_crt_statistics_http1_channel_init(struct aws_crt_statistics_http1_channel *stats);

/**
 * Cleans up a http channel handler statistics struct
 */
AWS_HTTP_API
void aws_crt_statistics_http1_channel_cleanup(struct aws_crt_statistics_http1_channel *stats);

/**
 * Resets a http channel handler statistics struct's statistics
 */
AWS_HTTP_API
void aws_crt_statistics_http1_channel_reset(struct aws_crt_statistics_http1_channel *stats);

/**
 * Initializes a HTTP/2 channel handler statistics struct
 */
AWS_HTTP_API
void aws_crt_statistics_http2_channel_init(struct aws_crt_statistics_http2_channel *stats);
/**
 * Resets a HTTP/2 channel handler statistics struct's statistics
 */
AWS_HTTP_API
void aws_crt_statistics_http2_channel_reset(struct aws_crt_statistics_http2_channel *stats);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_STATISTICS_H */
