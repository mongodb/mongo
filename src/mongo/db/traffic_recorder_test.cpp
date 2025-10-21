/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/traffic_recorder.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>

namespace mongo {
namespace {

class MockSessionWithBSON : public transport::MockSession {
public:
    explicit MockSessionWithBSON(HostAndPort remote,
                                 SockAddr remoteAddr,
                                 SockAddr localAddr,
                                 transport::TransportLayer* tl)
        : MockSession(remote, remoteAddr, localAddr, tl) {}

    void appendToBSON(BSONObjBuilder& bb) const override {
        bb.append("name", "mockSession");
    }
};

class TrafficRecorderTestUtil {
public:
    static StartTrafficRecording makeRecordingOptions(
        const std::string destination = "test_destination",
        memory_util::MemorySize maxFileSize = memory_util::MemorySize(2048),
        memory_util::MemorySize maxMemUsage = memory_util::MemorySize(2048)) {
        StartTrafficRecording options;
        options.setDestination(destination);
        options.setMaxFileSize(maxFileSize);
        options.setMaxMemUsage(maxMemUsage);
        return options;
    }

    static void verifyRecordedOffsets(TrafficRecorderForTest& recorder,
                                      const std::vector<long long>& expectedOffsets) {
        auto recording = recorder.getCurrentRecording();
        ASSERT(recording) << "Expected recording to exist, but was null.";

        auto recordedPackets = recording->getPcqPipe().consumer.popMany().first;

        ASSERT_EQ(recordedPackets.size(), expectedOffsets.size())
            << "Unexpected number of packets recorded: " << recordedPackets.size();

        size_t index = 0;
        for (const auto& packet : recordedPackets) {
            ASSERT_EQ(durationCount<Microseconds>(packet.offset), expectedOffsets[index])
                << "Incorrect offset for recorded packet at index " << index;
            index++;
        }
    }
};


TEST(TrafficRecorderTest, CorrectOffsets) {
    TrafficRecorderForTest trafficRecorder;
    StartTrafficRecording recordingOptions = TrafficRecorderTestUtil::makeRecordingOptions();

    gTrafficRecordingDirectory = "test_directory";

    auto ctx = ServiceContext::make(std::make_unique<ClockSourceMock>(),
                                    std::make_unique<ClockSourceMock>(),
                                    std::make_unique<TickSourceMock<Microseconds>>());

    auto& mockClock = static_cast<TickSourceMock<Microseconds>&>(*ctx->getTickSource());
    mockClock.reset(0);


    trafficRecorder.start(recordingOptions, ctx.get());


    auto mockSession =
        std::make_shared<MockSessionWithBSON>(HostAndPort(), SockAddr(), SockAddr(), nullptr);

    Message message = Message();
    message.setData(dbQuery, "test_query");

    trafficRecorder.observe(mockSession, message, ctx.get());

    mockClock.advance(Microseconds(500));

    trafficRecorder.observe(mockSession, message, ctx.get());

    mockClock.advance(Milliseconds(10));

    trafficRecorder.observe(mockSession, message, ctx.get());

    mockClock.advance(Microseconds(1));

    trafficRecorder.observe(mockSession, message, ctx.get());

    mockClock.advance(Seconds(1'000'000'000));

    trafficRecorder.observe(mockSession, message, ctx.get());

    TrafficRecorderTestUtil::verifyRecordedOffsets(trafficRecorder,
                                                   {0, 500, 10500, 10501, 1'000'000'000'010'501});

    trafficRecorder.stop(ctx.get());
}

}  // namespace
}  // namespace mongo
