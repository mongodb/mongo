/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/rpc/factory.h"

#include "mongo/rpc/command_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/protocol.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

std::unique_ptr<RequestBuilderInterface> makeRequestBuilder(ProtocolSet clientProtos,
                                                            ProtocolSet serverProtos) {
    return makeRequestBuilder(uassertStatusOK(negotiate(clientProtos, serverProtos)));
}

std::unique_ptr<RequestBuilderInterface> makeRequestBuilder(Protocol proto) {
    switch (proto) {
        case Protocol::kOpQuery:
            return stdx::make_unique<LegacyRequestBuilder>();
        case Protocol::kOpCommandV1:
            return stdx::make_unique<CommandRequestBuilder>();
        default:
            MONGO_UNREACHABLE;
    }
}

std::unique_ptr<ReplyInterface> makeReply(const Message* unownedMessage) {
    switch (unownedMessage->operation()) {
        case mongo::opReply:
            return stdx::make_unique<LegacyReply>(unownedMessage);
        case mongo::dbCommandReply:
            return stdx::make_unique<CommandReply>(unownedMessage);
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage->operation());
    }
}

std::unique_ptr<RequestInterface> makeRequest(const Message* unownedMessage) {
    switch (unownedMessage->operation()) {
        case mongo::dbQuery:
            return stdx::make_unique<LegacyRequest>(unownedMessage);
        case mongo::dbCommand:
            return stdx::make_unique<CommandRequest>(unownedMessage);
        default:
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Received a reply message with unexpected opcode: "
                                    << unownedMessage->operation());
    }
}

std::unique_ptr<ReplyBuilderInterface> makeReplyBuilder(Protocol protocol) {
    switch (protocol) {
        case Protocol::kOpQuery:
            return stdx::make_unique<LegacyReplyBuilder>();
        case Protocol::kOpCommandV1:
            return stdx::make_unique<CommandReplyBuilder>();
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace rpc
}  // namespace mongo
