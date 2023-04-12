/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/transport/grpc/channel_pool.h"

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::transport::grpc {
namespace {

MONGO_FAIL_POINT_DEFINE(blockBeforeCreatingNewChannel);
MONGO_FAIL_POINT_DEFINE(blockBeforeCreatingNewStub);

class ChannelPoolTest : public unittest::Test {
public:
    class DummyChannel {};
    class DummyStub {};
    using PoolType = ChannelPool<DummyChannel, DummyStub>;

    void setUp() override {
        _clockSource = std::make_unique<ClockSourceMock>();
        _pool = std::make_shared<PoolType>(
            _clockSource.get(),
            [this](ConnectSSLMode mode) { return _resolveSSLMode(mode); },
            [this](HostAndPort& remote, bool useSSL) { return _makeChannel(remote, useSSL); },
            [this](DummyChannel& channel) { return _makeStub(channel); });
    }

    void tearDown() override {
        _pool.reset();
        _clockSource.reset();
    }

    auto& clockSource() {
        return *_clockSource;
    }

    void setSSLMode(bool enable) {
        _sslMode.store(enable);
    }

    auto& pool() {
        return *_pool;
    }

private:
    bool _resolveSSLMode(ConnectSSLMode mode) {
        auto sslMode = _sslMode.load();
        ASSERT_TRUE(mode == ConnectSSLMode::kDisableSSL || sslMode);
        if (mode == ConnectSSLMode::kGlobalSSLMode)
            return sslMode;
        return mode == ConnectSSLMode::kEnableSSL;
    }

    DummyChannel _makeChannel(HostAndPort&, bool) {
        blockBeforeCreatingNewChannel.pauseWhileSet();
        return {};
    }

    DummyStub _makeStub(DummyChannel&) {
        blockBeforeCreatingNewStub.pauseWhileSet();
        return {};
    }

    std::unique_ptr<ClockSourceMock> _clockSource;
    std::shared_ptr<PoolType> _pool;
    AtomicWord<bool> _sslMode{false};
};

TEST_F(ChannelPoolTest, StartsEmpty) {
    ASSERT_EQ(pool().size(), 0);
}

TEST_F(ChannelPoolTest, CanReuseChannel) {
    HostAndPort remote("FakeHost", 123);
    auto s1 = pool().createStub(remote, ConnectSSLMode::kDisableSSL);
    ASSERT_EQ(pool().size(), 1);
    auto s2 = pool().createStub(remote, ConnectSSLMode::kDisableSSL);
    ASSERT_EQ(pool().size(), 1);
}

TEST_F(ChannelPoolTest, ConsidersSSLMode) {
    setSSLMode(true);
    ON_BLOCK_EXIT([&] { setSSLMode(false); });
    HostAndPort remote("FakeHost", 123);
    auto s1 = pool().createStub(remote, ConnectSSLMode::kEnableSSL);
    ASSERT_EQ(pool().size(), 1);
    auto s2 = pool().createStub(remote, ConnectSSLMode::kDisableSSL);
    ASSERT_EQ(pool().size(), 2);
}

TEST_F(ChannelPoolTest, DropUnusedChannel) {
    {
        // Create a new stub and immediately discard it. This should internally create a new
        // channel to `SomeHost:123`.
        pool().createStub({"SomeHost", 123}, ConnectSSLMode::kDisableSSL);
    }
    ASSERT_EQ(pool().size(), 1);
    clockSource().advance(Minutes{5});
    ASSERT_EQ(pool().dropIdleChannels(Minutes{5}), 1);
    ASSERT_EQ(pool().size(), 0);
}

TEST_F(ChannelPoolTest, UpdatesLastUsed) {
    {
        auto stub = pool().createStub({"Mongo", 123}, ConnectSSLMode::kDisableSSL);
        ASSERT_EQ(pool().size(), 1);
        // Advance time before destroying `stub` to update the last-used-time for the channel. The
        // stub, which is the only active user of its channel, is removed as we leave this scope.
        clockSource().advance(Minutes{1});
    }
    clockSource().advance(Minutes{4});
    ASSERT_EQ(pool().dropIdleChannels(Minutes{5}), 0);
    ASSERT_EQ(pool().size(), 1);
}

TEST_F(ChannelPoolTest, DropNotRecentlyUsedChannelsWithoutStubs) {
    HostAndPort remoteA("RemoteA", 123), remoteB("RemoteB", 123);
    auto s1 = pool().createStub(remoteA, ConnectSSLMode::kDisableSSL);
    {
        // Create a new stub and immediately discard it. This creates a new channel to `remoteB`.
        pool().createStub(remoteB, ConnectSSLMode::kDisableSSL);
    }
    ASSERT_EQ(pool().size(), 2);
    clockSource().advance(Minutes{2});
    ASSERT_EQ(pool().dropIdleChannels(Minutes{2}), 1);

    // Verifying that remoteA's channel remains open.
    auto s2 = pool().createStub(remoteA, ConnectSSLMode::kDisableSSL);
    ASSERT_EQ(pool().size(), 1);
}

TEST_F(ChannelPoolTest, DropAllChannelsWithNoStubs) {
    const auto kNumChannels = 10;
    for (int i = 1; i <= kNumChannels; i++) {
        // Each iteration results in creating a new channel, targeting "FakeHost:(123 + i)".
        pool().createStub({"FakeHost", 123 + i}, ConnectSSLMode::kDisableSSL);
    }
    ASSERT_EQ(pool().size(), kNumChannels);
    ASSERT_EQ(pool().dropAllChannels(), kNumChannels);
    ASSERT_EQ(pool().size(), 0);
}

DEATH_TEST_F(ChannelPoolTest, DropAllChannelsWithStubs, "invariant") {
    const auto kNumChannels = 10;
    for (int i = 1; i <= kNumChannels; i++) {
        auto stub = pool().createStub({"FakeHost", 123 + i}, ConnectSSLMode::kDisableSSL);
        if (i == kNumChannels) {
            ASSERT_EQ(pool().size(), kNumChannels);
            pool().dropAllChannels();  // Must be fatal.
        }
    }
}

TEST_F(ChannelPoolTest, CannotDropIdleChannelWhileCreatingNewStub) {
    unittest::Barrier beforeCreatingStub(2);
    stdx::thread worker([&] {
        beforeCreatingStub.countDownAndWait();
        auto stub = pool().createStub({"FakeHost", 123}, ConnectSSLMode::kDisableSSL);
    });
    ON_BLOCK_EXIT([&] { worker.join(); });

    FailPointEnableBlock fp("blockBeforeCreatingNewChannel");
    beforeCreatingStub.countDownAndWait();
    fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    // At this point, `worker` is blocked on the creation of a new channel, which should have
    // already been added to the list of open channels.
    ASSERT_EQ(pool().size(), 1);
    clockSource().advance(Minutes{2});
    ASSERT_EQ(pool().dropIdleChannels(Minutes{1}), 0);
}

TEST_F(ChannelPoolTest, OneChannelForMultipleStubs) {
    const HostAndPort remote{"SomeHost", 1234};
    unittest::Barrier beforeCreatingFirstStub(2);
    unittest::Barrier beforeCreatingSecondStub(2);
    stdx::thread channelCreator([&] {
        beforeCreatingFirstStub.countDownAndWait();
        // We create this one first, which should also create the underlying channel.
        auto stub1 = pool().createStub(remote, ConnectSSLMode::kDisableSSL);
    });
    stdx::thread channelUser([&] {
        beforeCreatingSecondStub.countDownAndWait();
        // This one is created second, which should reuse the created channel.
        auto stub2 = pool().createStub(remote, ConnectSSLMode::kDisableSSL);
    });
    ON_BLOCK_EXIT([&] {
        channelCreator.join();
        channelUser.join();
    });

    FailPointEnableBlock sFP("blockBeforeCreatingNewStub");
    {
        FailPointEnableBlock cFP("blockBeforeCreatingNewChannel");
        beforeCreatingFirstStub.countDownAndWait();
        cFP->waitForTimesEntered(cFP.initialTimesEntered() + 1);
        // `channelCreator` is now blocked in the factory function for creating new channels.
        beforeCreatingSecondStub.countDownAndWait();
        // `channelUser` can now go ahead with creating `stub2`, but it should wait for
        // `channelCreator` to return from creating the new channel.
    }
    sFP->waitForTimesEntered(sFP.initialTimesEntered() + 2);
    ASSERT_EQ(pool().size(), 1);
}

}  // namespace
}  // namespace mongo::transport::grpc
