#ifndef AWS_IO_STATISTICS_H
#define AWS_IO_STATISTICS_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

#include <aws/common/statistics.h>
#include <aws/io/tls_channel_handler.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_crt_io_statistics_category {
    AWSCRT_STAT_CAT_SOCKET = AWS_CRT_STATISTICS_CATEGORY_BEGIN_RANGE(AWS_C_IO_PACKAGE_ID),
    AWSCRT_STAT_CAT_TLS,
};

/**
 * Socket channel handler statistics record
 */
struct aws_crt_statistics_socket {
    aws_crt_statistics_category_t category;
    uint64_t bytes_read;
    uint64_t bytes_written;
};

/**
 * Tls channel handler statistics record
 */
struct aws_crt_statistics_tls {
    aws_crt_statistics_category_t category;
    uint64_t handshake_start_ns;
    uint64_t handshake_end_ns;
    enum aws_tls_negotiation_status handshake_status;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes socket channel handler statistics
 */
AWS_IO_API
int aws_crt_statistics_socket_init(struct aws_crt_statistics_socket *stats);

/**
 * Cleans up socket channel handler statistics
 */
AWS_IO_API
void aws_crt_statistics_socket_cleanup(struct aws_crt_statistics_socket *stats);

/**
 * Resets socket channel handler statistics for the next gather interval.  Calculate-once results are left alone.
 */
AWS_IO_API
void aws_crt_statistics_socket_reset(struct aws_crt_statistics_socket *stats);

/**
 * Initializes tls channel handler statistics
 */
AWS_IO_API
int aws_crt_statistics_tls_init(struct aws_crt_statistics_tls *stats);

/**
 * Cleans up tls channel handler statistics
 */
AWS_IO_API
void aws_crt_statistics_tls_cleanup(struct aws_crt_statistics_tls *stats);

/**
 * Resets tls channel handler statistics for the next gather interval.  Calculate-once results are left alone.
 */
AWS_IO_API
void aws_crt_statistics_tls_reset(struct aws_crt_statistics_tls *stats);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_STATISTICS_H */
