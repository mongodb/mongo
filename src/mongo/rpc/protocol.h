/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <fmt/format.h>

#include "mongo/rpc/message.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

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

inline Protocol protocolForMessage(const Message& message) {
    switch (message.operation()) {
        case mongo::dbMsg:
            return Protocol::kOpMsg;
        case mongo::dbQuery:
            return Protocol::kOpQuery;
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      fmt::format("Received a reply message with unexpected opcode: {}",
                                  message.operation()));
    }
}

}  // namespace rpc
}  // namespace mongo
