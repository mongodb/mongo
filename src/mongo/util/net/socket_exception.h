// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
enum class SocketErrorKind {
    CLOSED,
    RECV_ERROR,
    SEND_ERROR,
    RECV_TIMEOUT,
    SEND_TIMEOUT,
    FAILED_STATE,
    CONNECT_ERROR
};

/**
 * Returns a Status with ErrorCodes::SocketException with a correctly formed message.
 */
Status makeSocketError(SocketErrorKind kind,
                       const std::string& server,
                       const std::string& extra = "");

// Using a macro to preserve file/line info from call site.
#define throwSocketError(...)                          \
    do {                                               \
        uassertStatusOK(makeSocketError(__VA_ARGS__)); \
        MONGO_UNREACHABLE;                             \
    } while (false)

using NetworkException = ExceptionFor<ErrorCategory::NetworkError>;

}  // namespace mongo
