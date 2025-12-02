/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/replay/replay_command.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/shared_buffer.h"

#include <exception>

namespace mongo {

OpMsgRequest ReplayCommand::fetchMsgRequest() const {
    try {
        return parseBody();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

Microseconds ReplayCommand::fetchRequestOffset() const {
    try {
        return parseOffset();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

uint64_t ReplayCommand::fetchRequestSessionId() const {
    return _packet.id;
}

uint64_t ReplayCommand::fetchMessageId() const {
    return _packet.order;
}

std::string ReplayCommand::toString() const {
    if (isSessionStart()) {
        return "{\"sessionStart\": 1}";
    }
    if (isSessionEnd()) {
        return "{\"sessionEnd\": 1}";
    }
    OpMsgRequest messageRequest = fetchMsgRequest();
    return messageRequest.body.toString();
}

OpMsgRequest ReplayCommand::parseBody() const {
    Message message;
    // TODO: SERVER-107809 setData here copies the message unnecessarily.
    // OpMsg should be changed to allow parsing from a "view" type.
    message.setData(dbMsg, _packet.message.data(), _packet.message.dataLen());
    OpMsg::removeChecksum(&message);
    // TODO: SERVER-109756 remove unused fields such as lsid.
    return rpc::opMsgRequestFromAnyProtocol(message);
}
Microseconds ReplayCommand::parseOffset() const {
    return _packet.offset;
}

int64_t ReplayCommand::parseSessionId() const {
    return _packet.id;
}

std::string ReplayCommand::parseOpType() const {
    if (_packet.eventType == EventType::kSessionStart) {
        return kSessionStartOpType;
    }
    if (_packet.eventType == EventType::kSessionEnd) {
        return kSessionEndOpType;
    }
    if (_packet.message.getNetworkOp() == dbMsg) {
        return std::string(parseBody().getCommandName());
    } else {
        return "legacy";
    }
}

bool ReplayCommand::isSessionStart() const {
    return _packet.eventType == EventType::kSessionStart;
}

bool ReplayCommand::isSessionEnd() const {
    return _packet.eventType == EventType::kSessionEnd;
}

std::pair<Microseconds, uint64_t> extractOffsetAndSessionFromCommand(const ReplayCommand& command) {
    const Microseconds offset = command.fetchRequestOffset();
    const uint64_t sessionId = command.fetchRequestSessionId();
    return {offset, sessionId};
}
}  // namespace mongo
