// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"

namespace mongo {
class Message;

namespace rpc {

/**
 * Immutable view of an OP_REPLY legacy-style command reply.
 */
class LegacyReply final : public ReplyInterface {
public:
    /**
     * Construct a Reply from a Message.
     * The underlying message MUST outlive the Reply.
     */
    explicit LegacyReply(const Message* message);

    /**
     * The result of executing the command.
     */
    const BSONObj& getCommandReply() const final;

    Protocol getProtocol() const final;

private:
    BSONObj _commandReply;
};

}  // namespace rpc
}  // namespace mongo
