// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
#include "mongo/util/modules.h"
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

    static TestReaderPacket sessionStart();

    static TestReaderPacket sessionEnd();

    static TestReaderPacket response(BSONObj body);

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
