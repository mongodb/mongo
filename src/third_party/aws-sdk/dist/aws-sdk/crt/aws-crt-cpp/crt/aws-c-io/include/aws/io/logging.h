#ifndef AWS_IO_LOGGING_H
#define AWS_IO_LOGGING_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

#include <aws/common/logging.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_log_channel;
struct aws_log_formatter;
struct aws_log_writer;

enum aws_io_log_subject {
    AWS_LS_IO_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_IO_PACKAGE_ID),
    AWS_LS_IO_EVENT_LOOP,
    AWS_LS_IO_SOCKET,
    AWS_LS_IO_SOCKET_HANDLER,
    AWS_LS_IO_TLS,
    AWS_LS_IO_ALPN,
    AWS_LS_IO_DNS,
    AWS_LS_IO_PKI,
    AWS_LS_IO_CHANNEL,
    AWS_LS_IO_CHANNEL_BOOTSTRAP,
    AWS_LS_IO_FILE_UTILS,
    AWS_LS_IO_SHARED_LIBRARY,
    AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
    AWS_LS_IO_STANDARD_RETRY_STRATEGY,
    AWS_LS_IO_PKCS11,
    AWS_LS_IO_PEM,
    AWS_IO_LS_LAST = AWS_LOG_SUBJECT_END_RANGE(AWS_C_IO_PACKAGE_ID)
};
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_LOGGING_H */
