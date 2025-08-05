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

#include "mongo/replay/test_packet.h"


namespace mongo {
TestReaderPacket::TestReaderPacket(TrafficReaderPacket base, Message ownedMessage)
    : TrafficReaderPacket(base), ownedMessage(ownedMessage) {}

TestReaderPacket TestReaderPacket::make(BSONObj command) {
    OpMsgBuilder builder;
    builder.setBody(command);
    auto message = builder.finish();
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::find(BSONObj filter, BSONObj projection) {
    OpMsgBuilder builder;
    BSONObj findCommand = BSON("find" << "test"
                                      << "$db"
                                      << "test"
                                      << "filter" << filter);
    if (!projection.isEmpty()) {
        findCommand = findCommand.addFields(BSON("projection" << projection));
    }
    builder.setBody(findCommand);
    auto message = builder.finish();
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
    OpMsgBuilder builder;
    BSONObj aggregateCommand =
        BSON("aggregate" << "test" << "$db" << "test" << "pipeline" << pipeline << "cursor"
                         << BSON("batchSize" << 100));
    builder.setBody(aggregateCommand);
    auto message = builder.finish();
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReaderPacket TestReaderPacket::del(BSONObj filter) {
    OpMsgBuilder builder;
    BSONObj deleteOp = BSON("q" << filter << "limit" << 1);
    BSONObj deleteCommand = BSON("delete" << "test"
                                          << "$db"
                                          << "test"
                                          << "deletes" << BSON_ARRAY(deleteOp));

    builder.setBody(deleteCommand);
    auto message = builder.finish();
    return TestReaderPacket(TrafficReaderPacket{.message = message.buf()}, message);
}

TestReplayCommand::TestReplayCommand(TestReaderPacket packet)
    : ReplayCommand(packet), ownedMessage(packet.ownedMessage) {}

}  // namespace mongo
