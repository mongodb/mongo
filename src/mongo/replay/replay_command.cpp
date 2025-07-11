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

#include <exception>

namespace mongo {

OpMsgRequest ReplayCommand::fetchMsgRequest() const {
    try {
        return parseBody(_bsonCommand);
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

Date_t ReplayCommand::fetchRequestTimestamp() const {
    try {
        return parseSeenTimestamp(_bsonCommand);
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, lastError.reason());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientFailedToProcessBSON, e.what());
    }
}

int64_t ReplayCommand::fetchRequestSessionId() const {
    try {
        return parseSessionId(_bsonCommand);
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

OpMsgRequest ReplayCommand::parseBody(BSONObj command) const {
    // "rawop": {
    //    "header": { messagelength: 339, requestid: 2, responseto: 0, opcode: 2004 },
    //    "body": BinData(0, ...),
    //  },
    //   "seen": {
    //     "sec": 63883941272,
    //     "nsec": 8
    //   },
    //   "session": {
    //     "remote": "127.0.0.1:54482",
    //     "local": "127.0.0.1:27017"
    //   },
    //   "order": 8,
    //   "seenconnectionnum": 3,
    //   "playedconnectionnum": 0,
    //   "generation": 0,
    //   "opType": "find" }

    tassert(ErrorCodes::InternalError,
            "Ill-formed document. rawop is not a valid field",
            command.hasField("rawop"));

    BSONElement rawop = command["rawop"];

    tassert(ErrorCodes::InternalError,
            "Ill-formed document. body is not a valid field",
            rawop.Obj().hasField("body"));

    BSONElement bodyElem = rawop["body"];
    int len = 0;
    const char* data = static_cast<const char*>(bodyElem.binDataClean(len));
    Message message;
    const auto layoutSize = sizeof(MsgData::Layout);
    message.setData(dbMsg, data + layoutSize, len - layoutSize);
    OpMsg::removeChecksum(&message);
    return rpc::opMsgRequestFromAnyProtocol(message);
}

Date_t ReplayCommand::parseSeenTimestamp(BSONObj command) const {

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed document. rawop is not a valid field",
            command.hasField("rawop"));

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed document. Seen is not a valid field",
            command.hasField("seen"));

    BSONElement seenElem = command["seen"];

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed recording document. `seen` does not have nested fields",
            seenElem.type() == BSONType::object);

    auto sec = seenElem["sec"].numberLong();
    auto nano = seenElem["nsec"].numberLong();
    // TODO SERVER-106702 will handle timestamps accordingly.
    static constexpr long long unixToInternal = 62135596800LL;
    uint64_t unixSeconds = sec - unixToInternal;
    unixSeconds += nano;
    return Date_t::fromMillisSinceEpoch(unixSeconds);
}

int64_t ReplayCommand::parseSessionId(BSONObj command) const {

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed document. rawop is not a valid field",
            command.hasField("rawop"));

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed document. Session id is not present",
            command.hasField("seenconnectionnum"));

    BSONElement sessionElem = command["seenconnectionnum"];

    tassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Ill-formed recording document. SessionId is not a scalar type",
            sessionElem.type() == BSONType::numberLong);

    return sessionElem.Long();
}

std::string ReplayCommand::parseOpType(BSONObj command) const {
    tassert(ErrorCodes::InternalError,
            "failed to parse bson commands, opType must be present",
            command.hasField("opType"));
    BSONElement opTypeElem = command["opType"];
    tassert(ErrorCodes::InternalError,
            "failed to parse bson commands, opType must be a string",
            opTypeElem.type() == BSONType::string);
    auto opType = opTypeElem.String();
    tassert(ErrorCodes::InternalError,
            "failed to parse bson commands, opType must not be empty",
            !opType.empty());
    return opType;
}

bool ReplayCommand::isStartRecording() const {
    return parseOpType(_bsonCommand) == "startTrafficRecording";
}

bool ReplayCommand::isStopRecording() const {
    return parseOpType(_bsonCommand) == "stopTrafficRecording";
}

}  // namespace mongo
