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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <chrono>
#include <cstdint>
#include <ratio>
#include <string>

namespace mongo {
class TestReaderPacket : public TrafficReaderPacket {
public:
    TestReaderPacket(TrafficReaderPacket base, Message ownedMessage);

    static TestReaderPacket make(BSONObj command);

    static TestReaderPacket find(BSONObj filter, BSONObj projection = {});

    static TestReaderPacket insert(BSONArray documents);

    static TestReaderPacket aggregate(BSONArray pipeline);

    static TestReaderPacket del(BSONObj filter);

    // TrafficReaderPacket has a non-owning constview; for tests pass around an owning
    // message to keep the pointed-to data alive.
    Message ownedMessage;
};

class TestReplayCommand : public ReplayCommand {
public:
    TestReplayCommand(TestReaderPacket packet);
    // As with TrafficReaderPacket, ReplayCommand is usually a view into
    // existing data. For tests, it's convenient to be able to pass around
    // owned data.
    Message ownedMessage;
};

namespace cmds {
static const auto startRecordingPkt = TestReaderPacket::make(BSON("sessionStart" << "1.0"));

static const auto stopRecordingPkt = TestReaderPacket::make(BSON("sessionEnd" << "1.0"));

// Helper for taking the relevant parts of a Packet as arguments - e.g.,
//  foobar({.id = 1, .date = now})
struct PacketArgs {
    uint64_t id = 1;
    Microseconds offset{
        durationCount<Microseconds>(std::chrono::steady_clock::now().time_since_epoch())};
};

inline TestReplayCommand start(PacketArgs args) {
    auto startRecording = startRecordingPkt;

    startRecording.id = args.id;
    startRecording.offset = args.offset;
    startRecording.eventType = EventType::kSessionStart;
    return TestReplayCommand{startRecording};
}

inline TestReplayCommand stop(PacketArgs args) {
    auto stopRecording = stopRecordingPkt;

    stopRecording.id = args.id;
    stopRecording.offset = args.offset;
    stopRecording.eventType = EventType::kSessionEnd;
    return TestReplayCommand{stopRecording};
}

template <class... OtherArgs>
auto find(PacketArgs args, OtherArgs&&... otherArgs) {
    auto packet = TestReaderPacket::find(std::forward<OtherArgs>(otherArgs)...);

    packet.id = args.id;
    packet.offset = args.offset;
    return TestReplayCommand{packet};
}

template <class... OtherArgs>
auto insert(PacketArgs args, OtherArgs&&... otherArgs) {
    auto packet = TestReaderPacket::insert(std::forward<OtherArgs>(otherArgs)...);

    packet.id = args.id;
    packet.offset = args.offset;
    return TestReplayCommand{packet};
}

template <class... OtherArgs>
auto aggregate(PacketArgs args, OtherArgs&&... otherArgs) {
    auto packet = TestReaderPacket::aggregate(std::forward<OtherArgs>(otherArgs)...);

    packet.id = args.id;
    packet.offset = args.offset;
    return TestReplayCommand{packet};
}

template <class... OtherArgs>
auto del(PacketArgs args, OtherArgs&&... otherArgs) {
    auto packet = TestReaderPacket::del(std::forward<OtherArgs>(otherArgs)...);

    packet.id = args.id;
    packet.offset = args.offset;
    return TestReplayCommand{packet};
}

}  // namespace cmds

}  // namespace mongo
