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

#include "mongo/replay/session_simulator.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/replay/mini_mock.h"
#include "mongo/replay/performance_reporter.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/replay/session_scheduler.h"
#include "mongo/replay/test_packet.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <memory>
#include <ratio>

namespace mongo {

class TestSessionSimulator : public SessionSimulator {
public:
    TestSessionSimulator()
        : SessionSimulator(std::make_unique<ReplayCommandExecutor>(),
                           std::make_unique<SessionScheduler>(),
                           std::make_unique<PerformanceReporter>("test.bin")) {}

    std::chrono::steady_clock::time_point now() const override {
        auto handle = nowHook.synchronize();
        return (*handle)();
    }

    void sleepFor(std::chrono::steady_clock::duration duration) const override {
        auto handle = sleepHook.synchronize();
        (*handle)(duration);
    }

    ~TestSessionSimulator() override {
        // Halt all worker threads, before mock functions are destroyed.
        shutdown();
    }

    using NowMockFunction = MiniMockFunction<std::chrono::steady_clock::time_point>;
    using SleepMockFunction = MiniMockFunction<void, std::chrono::steady_clock::duration>;
    mutable synchronized_value<NowMockFunction> nowHook{NowMockFunction{"now"}};
    mutable synchronized_value<SleepMockFunction> sleepHook{SleepMockFunction{"sleepFor"}};
};

// Helper to allow adding mongo::Duration and std::chrono::duration.
// Used in this file for:
//    someDuration + 1s;
// In these cases, the result is desired to be a mongo::Duration.
template <class MongoDur, class Rep, class Period>
requires duration_detail::isMongoDuration<MongoDur>
auto operator+(const MongoDur& mongoDuration,
               const std::chrono::duration<Rep, Period>& nonMongoDuration) {
    return mongoDuration + mongo::duration_cast<MongoDur>(nonMongoDuration);
}
TEST(SessionSimulatorTest, TestSimpleCommandNoWait) {

    auto packet = TestReaderPacket::find(BSON("name" << "Alice"));

    std::string jsonStr = R"([{
    "_id": "681cb423980b72695075137f",
    "name": "Alice",
    "age": 30,
    "city": "New York"}])";
    ReplayTestServer server{{"find"}, {jsonStr}};

    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator;

        // connect to server with time
        const auto uri = server.getConnectionString();
        auto begin = std::chrono::steady_clock::now();
        auto eventOffset = Microseconds(0);
        // For the next call to now(), report the timestamp the replay started at.
        sessionSimulator.nowHook->ret(begin);

        // Recording and session both start "now".
        sessionSimulator.start(uri, begin, eventOffset);

        using namespace std::chrono_literals;
        eventOffset += Duration<std::milli>(1000);
        packet.offset = eventOffset;
        ReplayCommand command{packet};
        // For the next call to now(), report the replay is 1s in - the same time the find should be
        // issued at.
        sessionSimulator.nowHook->ret(begin + 1s);
        sessionSimulator.run(command, eventOffset);
    }

    BSONObj response = fromjson(jsonStr);
    ASSERT_TRUE(server.checkResponse("find", response));
}

TEST(SessionSimulatorTest, TestSimpleCommandWait) {
    auto packet = TestReaderPacket::find(BSON("name" << "Alice"));

    std::string jsonStr = R"([{
    "_id": "681cb423980b72695075137f",
    "name": "Alice",
    "age": 30,
    "city": "New York"}])";
    ReplayTestServer server{{"find"}, {jsonStr}};

    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator;

        // connect to server with time
        const auto uri = server.getConnectionString();
        auto begin = std::chrono::steady_clock::now();

        using namespace std::chrono_literals;

        // The session start occurred two seconds into the recording.
        auto eventOffset = Duration<std::milli>(2000);  // 2 seconds into the recording

        // For the first call to now() return the same timepoint the replay started at.
        sessionSimulator.nowHook->ret(begin);
        // Expect the simulator to try sleep for 2 seconds.
        sessionSimulator.sleepHook->expect(2s);

        sessionSimulator.start(uri, begin, eventOffset);


        // Issue a find request at 5s into the recording
        eventOffset = Duration<std::milli>(5000);
        packet.offset = eventOffset;
        ReplayCommand command{packet};

        // Report "now" as if time has advanced to when the session started.
        sessionSimulator.nowHook->ret(begin + 2s);
        // Simulator should attempt to sleep the remaining time to when the
        // find request was issued.
        sessionSimulator.sleepHook->expect(3s);

        sessionSimulator.run(command, eventOffset);
    }

    BSONObj response = fromjson(jsonStr);
    ASSERT_TRUE(server.checkResponse("find", response));
}

TEST(SessionSimulatorTest, TestSimpleCommandNoWaitTimeInThePast) {
    // Simulate a real scenario where time is in the past. No wait should happen.
    auto packet = TestReaderPacket::find(BSON("name" << "Alice"));

    std::string jsonStr = R"([{
    "_id": "681cb423980b72695075137f",
    "name": "Alice",
    "age": 30,
    "city": "New York"}])";
    ReplayTestServer server{{"find"}, {jsonStr}};

    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator;

        // connect to server with time
        const auto uri = server.getConnectionString();
        auto begin = stdx::chrono::steady_clock::now();
        using namespace std::chrono_literals;
        auto eventOffset =
            Duration<std::milli>(1000);  // A session started one second into the recording

        // Pretend the replay is actually *10* seconds into the replay.
        // That means it is now "late" starting this session, so should not sleep.
        sessionSimulator.nowHook->ret(begin + 10s);

        sessionSimulator.start(uri, begin, eventOffset);

        eventOffset = Duration<std::milli>(2000);
        packet.offset = eventOffset;
        ReplayCommand command{packet};

        // Replay is also "late" trying to replay this find, so should not sleep.
        sessionSimulator.nowHook->ret(begin + 10s);
        sessionSimulator.run(command, eventOffset);
    }

    BSONObj response = fromjson(jsonStr);
    ASSERT_TRUE(server.checkResponse("find", response));
}

}  // namespace mongo
