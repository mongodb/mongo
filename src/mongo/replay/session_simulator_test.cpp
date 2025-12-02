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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/replay/mini_mock.h"
#include "mongo/replay/performance_reporter.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/replay/test_packet.h"
#include "mongo/replay/traffic_recording_iterator.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <chrono>
#include <memory>

#include <boost/filesystem/path.hpp>

namespace mongo {

using namespace std::chrono_literals;

static const std::string fakeResponse = R"([{
    "_id": "681cb423980b72695075137f",
    "name": "Alice",
    "age": 30,
    "city": "New York"}])";

class TestSessionSimulator : public SessionSimulator {
public:
    using SessionSimulator::SessionSimulator;

    TestSessionSimulator(PacketSource source,
                         std::chrono::steady_clock::time_point startTime,
                         StringData uri)
        : SessionSimulator(std::move(source),
                           0 /* sessionID */,
                           startTime,
                           std::string(uri),
                           std::make_unique<ReplayCommandExecutor>(),
                           std::make_unique<PerformanceReporter>(uri, "test.bin")) {}

    std::chrono::steady_clock::time_point now() const override {
        auto handle = nowHook.synchronize();
        return (*handle)();
    }

    void sleepFor(std::chrono::steady_clock::duration duration) const override {
        auto handle = sleepHook.synchronize();
        (*handle)(duration);
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

class TestPackets {
public:
    struct Args {
        std::chrono::seconds offset;
        TestReaderPacket packet;
    };

    TestPackets& operator+=(Args&& args) {
        args.packet.offset = mongo::duration_cast<Microseconds>(args.offset);
        packets.push_back(std::move(args.packet));
        return *this;
    }

    operator PacketSource() const;

    const auto& operator[](size_t idx) {
        return packets[idx];
    }

    Date_t recordingStartTime = Date_t::now();
    std::vector<TestReaderPacket> packets;
};

TrafficRecordingPacket toOwned(const TrafficReaderPacket& packet) {
    Message ownedMessage;
    ownedMessage.setData(
        packet.message.getNetworkOp(), packet.message.data(), packet.message.dataLen());

    return {
        .eventType = packet.eventType,
        .id = packet.id,
        .session = std::string(packet.session),
        .offset = packet.offset,
        .order = packet.order,
        .message = std::move(ownedMessage),
    };
}

class TestFiles : public FileSet {
public:
    TestFiles(std::vector<TestReaderPacket> packets) {
        PacketWriter writer;
        const boost::filesystem::path filename = UUID::gen().toString() + ".bin";
        writer.open(filename);

        for (const auto& packet : packets) {
            writer.writePacket(toOwned(packet));
        }

        writer.close();

        files = {std::make_shared<boost::iostreams::mapped_file_source>(filename.string())};
    }
    /**
     * Acquire (read only) memory mapped access to the file at `index`.
     *
     * If the file is already mapped, shared access will be provided to the same map;
     * it will not be blindly re-mapped.
     */
    std::shared_ptr<boost::iostreams::mapped_file_source> get(size_t index) override {
        if (index >= files.size()) {
            return nullptr;
        }
        return files[index];
    }

    /**
     * Check if this FileSet contains any files.
     */
    bool empty() const override {
        return files.empty();
    }

    std::vector<std::shared_ptr<boost::iostreams::mapped_file_source>> files;
};

TestPackets::operator PacketSource() const {
    return PacketSource(std::make_shared<TestFiles>(packets));
}

TEST(SessionSimulatorTest, TestSimpleCommandNoWait) {
    ReplayTestServer server{{"find"}, {fakeResponse}};

    auto packet = TestReaderPacket::find(BSON("name" << "Alice"));

    auto replayStartTime = std::chrono::steady_clock::now();

    TestPackets packets;

    // Simulate a find command occurring 1 second into the recording.
    packets += {1s, TestReaderPacket::find(BSON("name" << "Alice"))};

    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator{
            packets, replayStartTime, server.getConnectionString()};

        // TODO SERVER-105627: First command will start session, and will call now() to
        // delay until the correct time. SessionStart events will explicitly do this.
        sessionSimulator.nowHook->ret(replayStartTime + 1s);

        // Initially report "now" as the exact time the find request needs to be issued.
        sessionSimulator.nowHook->ret(replayStartTime + 1s);
        // Don't expect any call to sleepFor.

        sessionSimulator.run();
    }
}

TEST(SessionSimulatorTest, TestSimpleCommandWait) {
    ReplayTestServer server{{"find"}, {fakeResponse}};

    auto replayStartTime = std::chrono::steady_clock::now();

    TestPackets packets;

    // Simulate a find command occurring 2 second into the recording.
    packets += {2s, TestReaderPacket::find(BSON("name" << "Alice"))};

    // Simulate another command, 3 seconds later (total offset of 5s into the recording).
    packets += {5s, TestReaderPacket::find(BSON("name" << "Alice"))};

    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator{
            packets, replayStartTime, server.getConnectionString()};

        // First find

        // Initially report "now" as the recording start time.
        sessionSimulator.nowHook->ret(replayStartTime);
        // Expect the simulator to try sleep for 2s.
        sessionSimulator.sleepHook->expect(2s);

        // Second find

        // Report "now" as if immediately after sleeping for the previous command.
        sessionSimulator.nowHook->ret(replayStartTime + 2s);
        // Expect the simulator to try sleep for the remaining 3s to reach the target offset time.
        sessionSimulator.sleepHook->expect(3s);

        sessionSimulator.run();
    }
}

TEST(SessionSimulatorTest, TestSimpleCommandNoWaitTimeInThePast) {
    ReplayTestServer server{{"find"}, {fakeResponse}};
    auto replayStartTime = std::chrono::steady_clock::now();

    TestPackets packets;

    // Simulate a find command occurring 1 second into the recording.
    packets += {1s, TestReaderPacket::find(BSON("name" << "Alice"))};


    // test simulator scoped in order to complete all the tasks.
    {
        TestSessionSimulator sessionSimulator{
            packets, replayStartTime, server.getConnectionString()};

        // TODO SERVER-105627: First command will start session, and will call now() to
        // delay until the correct time. SessionStart events will explicitly do this.
        sessionSimulator.nowHook->ret(replayStartTime + 10s);

        // Initially report "now" as _later than_ the command should have run.
        sessionSimulator.nowHook->ret(replayStartTime + 10s);
        // Don't expect any call to sleepFor.

        sessionSimulator.run();
    }
}

}  // namespace mongo
