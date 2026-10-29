// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    auto& session = *mockSession;

    Message message = Message();
    message.setData(dbQuery, "test_query");

    trafficRecorder.observe(session, message, EventType::kRequest);

    mockClock.advance(Microseconds(500));

    trafficRecorder.observe(session, message, EventType::kResponse);

    mockClock.advance(Milliseconds(10));

    trafficRecorder.observe(session, message, EventType::kRequest);

    mockClock.advance(Microseconds(1));

    trafficRecorder.observe(session, message, EventType::kResponse);

    mockClock.advance(Seconds(1'000'000'000));

    trafficRecorder.observe(session, message, EventType::kRequest);

    TrafficRecorderTestUtil::verifyRecordedOffsets(trafficRecorder,
                                                   {0, 500, 10500, 10501, 1'000'000'000'010'501});

    trafficRecorder.stop(ctx.get());
}

}  // namespace
}  // namespace mongo
