/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/statistics.h>

int aws_crt_statistics_http1_channel_init(struct aws_crt_statistics_http1_channel *stats) {
    AWS_ZERO_STRUCT(*stats);
    stats->category = AWSCRT_STAT_CAT_HTTP1_CHANNEL;

    return AWS_OP_SUCCESS;
}

void aws_crt_statistics_http1_channel_cleanup(struct aws_crt_statistics_http1_channel *stats) {
    (void)stats;
}

void aws_crt_statistics_http1_channel_reset(struct aws_crt_statistics_http1_channel *stats) {
    stats->pending_outgoing_stream_ms = 0;
    stats->pending_incoming_stream_ms = 0;
    stats->current_outgoing_stream_id = 0;
    stats->current_incoming_stream_id = 0;
}

void aws_crt_statistics_http2_channel_init(struct aws_crt_statistics_http2_channel *stats) {
    AWS_ZERO_STRUCT(*stats);
    stats->category = AWSCRT_STAT_CAT_HTTP2_CHANNEL;
}

void aws_crt_statistics_http2_channel_reset(struct aws_crt_statistics_http2_channel *stats) {
    stats->pending_outgoing_stream_ms = 0;
    stats->pending_incoming_stream_ms = 0;
    stats->was_inactive = false;
}
