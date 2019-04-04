/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/rpc/factory.h"

#include "merizo/rpc/legacy_reply.h"
#include "merizo/rpc/legacy_reply_builder.h"
#include "merizo/rpc/legacy_request.h"
#include "merizo/rpc/legacy_request_builder.h"
#include "merizo/rpc/message.h"
#include "merizo/rpc/op_msg_rpc_impls.h"
#include "merizo/rpc/protocol.h"
#include "merizo/stdx/memory.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/merizoutils/str.h"

namespace merizo {
namespace rpc {

Message messageFromOpMsgRequest(Protocol proto, const OpMsgRequest& request) {
    switch (proto) {
        case Protocol::kOpMsg:
            return request.serialize();
        case Protocol::kOpQuery:
            return legacyRequestFromOpMsgRequest(request);
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<ReplyInterface> makeReply(const Message* unownedMessage) {
    switch (unownedMessage->operation()) {
        case merizo::dbMsg:
            return stdx::make_unique<OpMsgReply>(OpMsg::parseOwned(*unownedMessage));
        case merizo::opReply:
            return stdx::make_unique<LegacyReply>(unownedMessage);
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage->operation());
    }
}

OpMsgRequest opMsgRequestFromAnyProtocol(const Message& unownedMessage) {
    switch (unownedMessage.operation()) {
        case merizo::dbMsg:
            return OpMsgRequest::parseOwned(unownedMessage);
        case merizo::dbQuery:
            return opMsgRequestFromLegacyRequest(unownedMessage);
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage.operation());
    }
}

std::unique_ptr<ReplyBuilderInterface> makeReplyBuilder(Protocol protocol) {
    switch (protocol) {
        case Protocol::kOpMsg:
            return stdx::make_unique<OpMsgReplyBuilder>();
        case Protocol::kOpQuery:
            return stdx::make_unique<LegacyReplyBuilder>();
    }
    MONGO_UNREACHABLE;
}

}  // namespace rpc
}  // namespace merizo
