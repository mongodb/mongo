// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <string>

namespace mongo {
using namespace std::literals::string_view_literals;
class ReplayCommand {
public:
    static constexpr auto kEventFieldName = "event"sv;

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

    EventType getEventType() const;

    void replaceBody(BSONObj buf);

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
    // During replay, it may be necessary to edit a message before executing it.
    // In that case, a new message may be stored in this shared buffer, and _packet
    // made to reference it.
    SharedBuffer _ownedBody;

    // Lazily initialized on first use, to avoid repeated parsing from packet data.
    mutable boost::optional<OpMsgRequest> _parsedRequest;
};

std::pair<Microseconds, uint64_t> extractOffsetAndSessionFromCommand(const ReplayCommand& command);

}  // namespace mongo
