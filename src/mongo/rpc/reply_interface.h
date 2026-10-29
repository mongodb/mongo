// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/protocol.h"
#include "mongo/util/modules.h"

namespace mongo {
class BSONObj;
class Message;

namespace rpc {

/**
 * An immutable view of an RPC Reply.
 */
class [[MONGO_MOD_OPEN]] ReplyInterface {
    ReplyInterface(const ReplyInterface&) = delete;
    ReplyInterface& operator=(const ReplyInterface&) = delete;

public:
    virtual ~ReplyInterface() = default;

    /**
     * The result of executing the command.
     */
    virtual const BSONObj& getCommandReply() const = 0;

    /**
     * Gets the protocol used to deserialize this reply. This should be used for validity
     * checks only - runtime behavior changes should be implemented with polymorphism.
     */
    virtual Protocol getProtocol() const = 0;

protected:
    ReplyInterface() = default;
};

}  // namespace rpc
}  // namespace mongo
