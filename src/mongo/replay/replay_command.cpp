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

Date_t ReplayCommand::fetchRequestTimestamp() const {
    try {
        return parseSeenTimestamp();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

int64_t ReplayCommand::fetchRequestSessionId() const {
    try {
        return parseSessionId();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

std::string ReplayCommand::toString() const {
    OpMsgRequest messageRequest = fetchMsgRequest();
    return messageRequest.body.toString();
}

OpMsgRequest ReplayCommand::parseBody() const {
    Message message;
    // TODO: SERVER-107809 setData here copies the message unnecessarily.
    // OpMsg should be changed to allow parsing from a "view" type.
    message.setData(dbMsg, _packet.message.data(), _packet.message.dataLen());
    OpMsg::removeChecksum(&message);
    return rpc::opMsgRequestFromAnyProtocol(message);
}

Date_t ReplayCommand::parseSeenTimestamp() const {
    return _packet.date;
}

int64_t ReplayCommand::parseSessionId() const {
    return _packet.id;
}

std::string ReplayCommand::parseOpType() const {
    if (_packet.message.getNetworkOp() == dbMsg) {
        return std::string(parseBody().getCommandName());
    } else {
        return "legacy";
    }
}

bool ReplayCommand::isStartRecording() const {
    return parseOpType() == "startTrafficRecording";
}

bool ReplayCommand::isStopRecording() const {
    return parseOpType() == "stopTrafficRecording";
}

std::pair<Date_t, int64_t> extractTimeStampAndSessionFromCommand(const ReplayCommand& command) {
    const Date_t timestamp = command.fetchRequestTimestamp();
    const int64_t sessionId = command.fetchRequestSessionId();
    return {timestamp, sessionId};
}

}  // namespace mongo
