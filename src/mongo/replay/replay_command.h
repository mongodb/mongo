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
#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <string>

namespace mongo {
class ReplayCommand {
public:
    static constexpr auto kEventFieldName = "event"_sd;

    explicit ReplayCommand(TrafficReaderPacket packet) : _packet(std::move(packet)) {}
    /*
     * Converts a command to replay into an internal server msg request that is used to execute the
     * command.
     */
    OpMsgRequest fetchMsgRequest() const;

    /** Extract only the timestamp. Useful for session simulation. */
    Microseconds fetchRequestOffset() const;

    /** Extract the session id for the current command */
    uint64_t fetchRequestSessionId() const;

    /** Unique id associated to the message present in the recording */
    uint64_t fetchMessageId() const;

    /** Use this method to know if the command represents a start recording, this starts the session
     * simulation. */
    bool isSessionStart() const;

    /** Use this method to know if the command represents a stop recording, this ends the session
     * simulation. */
    bool isSessionEnd() const;

    /** Mostly for debugging purposes, converts the replay command to string. */
    std::string toString() const;

    std::string parseOpType() const;

private:
    /** Extract the actual message body containing the actual bson command containing the query */
    OpMsgRequest parseBody() const;

    /*
     * Extract timestamp of when the command was recorded on the server and use it for deciding
     * whether to replay the command or not
     */
    Microseconds parseOffset() const;

    /*
     * Extract sessionId. Used for pinning the command to a session simulator
     */
    int64_t parseSessionId() const;


    TrafficReaderPacket _packet;
};

std::pair<Microseconds, int64_t> extractOffsetAndSessionFromCommand(const ReplayCommand& command);

}  // namespace mongo
