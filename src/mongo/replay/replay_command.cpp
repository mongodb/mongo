// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/replay_command.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/shared_buffer.h"

#include <chrono>
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
    if (!_parsedRequest) {
        Message message;
        // TODO: SERVER-107809 setData here copies the message unnecessarily.
        // OpMsg should be changed to allow parsing from a "view" type.
        message.setData(dbMsg, _packet.message.data(), _packet.message.dataLen());
        OpMsg::removeChecksum(&message);
        // TODO: SERVER-109756 remove unused fields such as lsid.
        _parsedRequest = rpc::opMsgRequestFromAnyProtocol(message);
    }
    return *_parsedRequest;
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

EventType ReplayCommand::getEventType() const {
    return _packet.eventType;
}

void ReplayCommand::replaceBody(BSONObj body) {
    parseBody();
    _parsedRequest->body = body;
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
