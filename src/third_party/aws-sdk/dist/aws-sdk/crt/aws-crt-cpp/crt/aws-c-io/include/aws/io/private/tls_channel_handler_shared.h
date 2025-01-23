#ifndef AWS_IO_TLS_CHANNEL_HANDLER_SHARED_H
#define AWS_IO_TLS_CHANNEL_HANDLER_SHARED_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

#include <aws/io/channel.h>
#include <aws/io/statistics.h>

struct aws_tls_connection_options;

struct aws_tls_channel_handler_shared {
    uint32_t tls_timeout_ms;
    struct aws_channel_handler *handler;
    struct aws_channel_task timeout_task;
    struct aws_crt_statistics_tls stats;
};

enum aws_tls_handler_read_state {
    AWS_TLS_HANDLER_OPEN,
    AWS_TLS_HANDLER_READ_SHUTTING_DOWN,
    AWS_TLS_HANDLER_READ_SHUT_DOWN_COMPLETE,
};

AWS_EXTERN_C_BEGIN

AWS_IO_API void aws_tls_channel_handler_shared_init(
    struct aws_tls_channel_handler_shared *tls_handler_shared,
    struct aws_channel_handler *handler,
    struct aws_tls_connection_options *options);

AWS_IO_API void aws_tls_channel_handler_shared_clean_up(struct aws_tls_channel_handler_shared *tls_handler_shared);

AWS_IO_API void aws_on_drive_tls_negotiation(struct aws_tls_channel_handler_shared *tls_handler_shared);

AWS_IO_API void aws_on_tls_negotiation_completed(
    struct aws_tls_channel_handler_shared *tls_handler_shared,
    int error_code);

/**
 * Returns true if an aws_byte_buf on aws_tls_ctx_options was set by the user.
 * Use this to determine whether a buf was set. DO NOT simply check if buf.len > 0.
 *
 * Reasoning:
 * If the user calls a setter function but passes a 0 length file or cursor, buf.len will be zero.
 * TLS should still respect the fact that the setter was called.
 * TLS should not use defaults instead just because length is 0.
 */
AWS_IO_API bool aws_tls_options_buf_is_set(const struct aws_byte_buf *buf);

AWS_EXTERN_C_END

#endif /* AWS_IO_TLS_CHANNEL_HANDLER_SHARED_H */
