// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <fmt/format.h>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] rpc {

/**
 * Bit flags representing support for a particular RPC protocol. This is just an internal
 * representation, and is never transmitted over the wire. It should never be used for any other
 * feature detection in favor of max/min wire version.
 *
 * The system only currently offers full support for the OP_MSG protocol. However, it can continue
 * to handle OP_QUERY in some limited cases, in particular for the hello/isMaster command sent by
 * clients on connection open.
 */
enum class Protocol : std::uint64_t {
    /**
     * The pre-3.6 OP_QUERY on db.$cmd protocol
     */
    kOpQuery = 1 << 0,

    /**
     * The 3.6+ OP_MSG protocol.
     */
    kOpMsg = 1 << 1,
};

inline Protocol protocolForOperation(NetworkOp operation) {
    switch (operation) {
        case mongo::dbMsg:
            return Protocol::kOpMsg;
        case mongo::dbQuery:
            return Protocol::kOpQuery;
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      fmt::format("Received a reply message with unexpected opcode: {}",
                                  fmt::underlying(operation)));
    }
}

inline Protocol protocolForMessage(const Message& message) {
    return protocolForOperation(message.operation());
}

}  // namespace rpc
}  // namespace mongo
