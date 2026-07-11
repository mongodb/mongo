// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/test_packet.h"

#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder_event.h"


namespace mongo {
TestReaderPacket::TestReaderPacket(TrafficReaderPacket base, Message ownedMessage)
    : TrafficReaderPacket(base), ownedMessage(ownedMessage) {}

auto makeMessage(BSONObj body = {}) {
    OpMsgBuilder builder;
    builder.setBody(body);
    return builder.finish();
}

TestReaderPacket TestReaderPacket::make(BSONObj command) {
    auto message = makeMessage(command);
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::sessionStart() {
    auto message = makeMessage();
    return TestReaderPacket(
        TrafficReaderPacket{.eventType = EventType::kSessionStart, .message = message.buf()},
        message);
}

TestReaderPacket TestReaderPacket::sessionEnd() {
    auto message = makeMessage();
    return TestReaderPacket(
        TrafficReaderPacket{.eventType = EventType::kSessionEnd, .message = message.buf()},
        message);
}

TestReaderPacket TestReaderPacket::response(BSONObj body) {
    auto message = makeMessage(body);
    return TestReaderPacket(
        TrafficReaderPacket{.eventType = EventType::kResponse, .message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::find(BSONObj filter, BSONObj projection) {
    BSONObj findCommand = BSON("find" << "test"
                                      << "$db"
                                      << "test"
                                      << "filter" << filter);
    if (!projection.isEmpty()) {
        findCommand = findCommand.addFields(BSON("projection" << projection));
    }
    auto message = makeMessage(findCommand);
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::insert(BSONArray documents) {
    OpMsgBuilder builder;
    BSONObj insertCommand = BSON("insert" << "test"
                                          << "$db"
                                          << "test");
    {
        auto dsb = builder.beginDocSequence("documents");
        for (const auto& doc : documents) {
            dsb.append(doc.Obj());
        }
    }
    builder.setBody(insertCommand);
    auto message = builder.finish();
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::aggregate(BSONArray pipeline) {
    BSONObj aggregateCommand =
        BSON("aggregate" << "test" << "$db" << "test" << "pipeline" << pipeline << "cursor"
                         << BSON("batchSize" << 100));
    auto message = makeMessage(aggregateCommand);
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::del(BSONObj filter) {
    BSONObj deleteOp = BSON("q" << filter << "limit" << 1);
    BSONObj deleteCommand = BSON("delete" << "test"
                                          << "$db"
                                          << "test"
                                          << "deletes" << BSON_ARRAY(deleteOp));

    auto message = makeMessage(deleteCommand);
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReplayCommand::TestReplayCommand(TestReaderPacket packet)
    : ReplayCommand(packet), ownedMessage(packet.ownedMessage) {}

}  // namespace mongo
