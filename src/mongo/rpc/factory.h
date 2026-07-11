// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/util/modules.h"

#include <memory>

/**
 * Utilities to construct the correct concrete rpc class based on what the remote server
 * supports, and what the client has been configured to do.
 */

namespace [[MONGO_MOD_PUBLIC]] mongo {
class Message;

namespace rpc {
class ReplyBuilderInterface;
class ReplyInterface;

/**
 * Returns the appropriate concrete RequestBuilder. Throws if one cannot be chosen.
 */
std::unique_ptr<ReplyInterface> makeReply(const Message* unownedMessage);

/**
 * Parses the message (from any protocol) into an OpMsgRequest.
 */
OpMsgRequest opMsgRequestFromAnyProtocol(const Message& unownedMessage, Client* client = nullptr);

/**
 * Returns the appropriate concrete ReplyBuilder.
 */
std::unique_ptr<ReplyBuilderInterface> makeReplyBuilder(Protocol protocol);

}  // namespace rpc
}  // namespace mongo
