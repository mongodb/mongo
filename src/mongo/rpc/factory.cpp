// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/factory.h"

#include "mongo/base/error_codes.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

namespace mongo {
namespace rpc {

std::unique_ptr<ReplyInterface> makeReply(const Message* unownedMessage) {
    switch (unownedMessage->operation()) {
        case mongo::dbMsg:
            return std::make_unique<OpMsgReply>(OpMsg::parseOwned(*unownedMessage));
        case mongo::opReply:
            return std::make_unique<LegacyReply>(unownedMessage);
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage->operation());
    }
}

OpMsgRequest opMsgRequestFromAnyProtocol(const Message& unownedMessage, Client* client) {
    switch (unownedMessage.operation()) {
        case mongo::dbMsg:
            return OpMsgRequest::parseOwned(unownedMessage, client);
        case mongo::dbQuery: {
            return opMsgRequestFromLegacyRequest(unownedMessage);
        }
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage.operation());
    }
}

std::unique_ptr<ReplyBuilderInterface> makeReplyBuilder(Protocol protocol) {
    switch (protocol) {
        case Protocol::kOpMsg:
            return std::make_unique<OpMsgReplyBuilder>();
        case Protocol::kOpQuery:
            return std::make_unique<LegacyReplyBuilder>();
    }
    MONGO_UNREACHABLE;
}

}  // namespace rpc
}  // namespace mongo
