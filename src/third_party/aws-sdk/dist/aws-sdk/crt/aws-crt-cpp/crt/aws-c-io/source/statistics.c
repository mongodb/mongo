/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/statistics.h>

#include <aws/io/channel.h>
#include <aws/io/logging.h>

int aws_crt_statistics_socket_init(struct aws_crt_statistics_socket *stats) {
    AWS_ZERO_STRUCT(*stats);
    stats->category = AWSCRT_STAT_CAT_SOCKET;

    return AWS_OP_SUCCESS;
}

void aws_crt_statistics_socket_cleanup(struct aws_crt_statistics_socket *stats) {
    (void)stats;
}

void aws_crt_statistics_socket_reset(struct aws_crt_statistics_socket *stats) {
    stats->bytes_read = 0;
    stats->bytes_written = 0;
}

int aws_crt_statistics_tls_init(struct aws_crt_statistics_tls *stats) {
    AWS_ZERO_STRUCT(*stats);
    stats->category = AWSCRT_STAT_CAT_TLS;
    stats->handshake_status = AWS_TLS_NEGOTIATION_STATUS_NONE;

    return AWS_OP_SUCCESS;
}

void aws_crt_statistics_tls_cleanup(struct aws_crt_statistics_tls *stats) {
    (void)stats;
}

void aws_crt_statistics_tls_reset(struct aws_crt_statistics_tls *stats) {
    /*
     * We currently don't have any resettable tls statistics yet, but they may be added in the future.
     */
    (void)stats;
}
