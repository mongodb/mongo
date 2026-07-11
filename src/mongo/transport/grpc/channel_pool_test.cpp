// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/channel_pool.h"

#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <memory>

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
            [this](const HostAndPort& remote, bool useSSL) { return _makeChannel(remote, useSSL); },
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

    DummyChannel _makeChannel(const HostAndPort&, bool) {
        blockBeforeCreatingNewChannel.pauseWhileSet();
        return {};
    }

    DummyStub _makeStub(DummyChannel&) {
        blockBeforeCreatingNewStub.pauseWhileSet();
        return {};
    }

    std::unique_ptr<ClockSourceMock> _clockSource;
    std::shared_ptr<PoolType> _pool;
    Atomic<bool> _sslMode{false};
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

TEST_F(ChannelPoolTest, DropChannelsByTarget) {
    constexpr auto numChannelsPerHost = 5;
    std::vector<std::string> hostnames = {"Host1", "Host2"};
    for (const auto& host : hostnames) {
        for (int port = 0; port < numChannelsPerHost; ++port) {
            pool().createStub({host, port}, ConnectSSLMode::kDisableSSL);
        }
    }

    auto numChannels = numChannelsPerHost * hostnames.size();
    ASSERT_EQ(pool().size(), numChannels);

    for (const auto& host : hostnames) {
        for (int port = 0; port < numChannelsPerHost; ++port) {
            ASSERT_EQ(pool().dropChannelsByTarget({host, port}), 1);
            ASSERT_EQ(pool().size(), --numChannels);
        }
    }
    ASSERT_EQ(pool().size(), 0);
}

TEST_F(ChannelPoolTest, DropNonExistingChannel) {
    pool().createStub({"Hostname", 111}, ConnectSSLMode::kDisableSSL);
    ASSERT_EQ(pool().size(), 1);

    // No channel is dropped since there is no channel associated with this host and port.
    ASSERT_EQ(pool().dropChannelsByTarget({"Hostname", 112}), 0);
    ASSERT_EQ(pool().size(), 1);
}

TEST_F(ChannelPoolTest, SetKeepOpen) {
    constexpr auto numChannels = 10;
    constexpr auto startingPort = 123;
    constexpr auto hostname = "FakeHost";
    for (int i = 0; i < numChannels; i++) {
        pool().createStub({hostname, startingPort + i}, ConnectSSLMode::kDisableSSL);
    }

    // Noop since there is no channel associated with this host and port.
    pool().setKeepOpen({hostname, startingPort - 1}, true);

    // setKeepOpen should prevent this channel from closing.
    pool().setKeepOpen({hostname, startingPort}, true);
    ASSERT_EQ(pool().dropAllChannels(), numChannels - 1);

    pool().setKeepOpen({hostname, startingPort}, false);
    ASSERT_EQ(pool().dropAllChannels(), 1);
    ASSERT_EQ(pool().size(), 0);
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
