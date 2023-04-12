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

#pragma once

#include <memory>
#include <vector>

#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

/**
 * Allows maintaining a pool of `Channel` objects and using them to create instances of `Stub`.
 * `Channel` and `Stub` are defined as template types to facilitate unit-testing.
 * This type is oblivious to how gRPC channels and stubs are created, and relies on the factory
 * functions (`ChannelFactory` and `StubFactory`) to handle that.
 */
template <class Channel, class Stub>
class ChannelPool : public std::enable_shared_from_this<ChannelPool<Channel, Stub>> {
public:
    using SSLModeResolver = unique_function<bool(ConnectSSLMode)>;
    using ChannelFactory = unique_function<Channel(HostAndPort&, bool)>;
    using StubFactory = unique_function<Stub(Channel&)>;

    /**
     * Maintains state for an individual `Channel`: allows deferred creation of `Channel` as well as
     * tracking its last-used-time.
     * All public APIs for this type are thread-safe.
     */
    class ChannelState {
    public:
        ChannelState(std::shared_ptr<ChannelPool> pool,
                     HostAndPort remote,
                     bool useSSL,
                     Future<Channel> channel)
            : _pool(std::move(pool)),
              _remote(std::move(remote)),
              _useSSL(useSSL),
              _channel(std::move(channel)) {}

        ChannelState(const ChannelState&) = delete;
        ChannelState& operator=(const ChannelState&) = delete;

        Channel& channel() {
            return _channel.get();
        }

        const HostAndPort& remote() const {
            return _remote;
        }

        bool useSSL() const {
            return _useSSL;
        }

        void updateLastUsed() {
            auto now = _pool->_clockSource->now();
            **_lastUsed = now;
        }

        Date_t lastUsed() const {
            return **_lastUsed;
        }

    private:
        std::shared_ptr<ChannelPool> _pool;
        const HostAndPort _remote;
        const bool _useSSL;
        Future<Channel> _channel;
        synchronized_value<Date_t> _lastUsed;
    };

    /**
     * RAII type for `Stub` that helps with identifying idle channels.
     * In terms of thread-safety, this type follows the semantics of `Stub`.
     */
    class StubHandle {
    public:
        explicit StubHandle(std::shared_ptr<ChannelState> channelState, Stub stub)
            : _channelState(std::move(channelState)), _stub(std::move(stub)) {}

        ~StubHandle() {
            _channelState->updateLastUsed();
        }

        Stub& stub() {
            return _stub;
        }

    private:
        std::shared_ptr<ChannelState> _channelState;
        Stub _stub;
    };

    /**
     * Constructs a new instance of `ChannelPool` and accepts the following:
     * - `clockSource` is used to record last-used-time for channels (doesn't need much accuracy).
     * - `sslModeResolver` translates `ConnectSSLMode` into a boolean that decides if an encrypted
     *   channel should be used to create new stubs.
     * - `channelFactory` is the factory for creating new channels.
     * - `stubFactory` is the factory for creating new stubs.
     */
    explicit ChannelPool(ClockSource* clockSource,
                         SSLModeResolver sslModeResolver,
                         ChannelFactory channelFactory,
                         StubFactory stubFactory)
        : _clockSource(clockSource),
          _sslModeResolver(std::move(sslModeResolver)),
          _channelFactory(std::move(channelFactory)),
          _stubFactory(std::move(stubFactory)) {}

    /**
     * Creates a new stub to `remote` that uses `sslMode`. Internally, an existing channel is used
     * to create the new stub, if available. Otherwise, a new channel is created.
     */
    std::unique_ptr<StubHandle> createStub(HostAndPort remote, ConnectSSLMode sslMode) {
        std::shared_ptr<ChannelState> cs = [&] {
            const auto useSSL = _sslModeResolver(sslMode);
            ChannelMapKeyType key{remote, useSSL};
            auto lk = stdx::unique_lock(_mutex);
            if (auto iter = _channels.find(key); iter != _channels.end()) {
                return iter->second;
            } else {
                auto pf = makePromiseFuture<Channel>();
                auto state = std::make_shared<ChannelState>(
                    this->shared_from_this(), remote, useSSL, std::move(pf.future));
                _channels.insert({key, state});
                lk.unlock();
                LOGV2_INFO(7401801,
                           "Creating a new gRPC channel",
                           "remote"_attr = remote,
                           "useSSL"_attr = useSSL);
                pf.promise.setWith([&] { return _channelFactory(remote, useSSL); });
                return state;
            }
        }();
        return std::make_unique<StubHandle>(std::move(cs), _stubFactory(cs->channel()));
    }

    /**
     * Drops all idle channels that are not used for the past `sinceLastUsed` minutes. An idle
     * channel is one that is not referenced by any instance of `StubHandle`. Returns the number of
     * dropped channels.
     * Internally, this will iterate through all channels in the pool. This should not have any
     * performance implications since we drop idle channels infrequently (e.g., every 30 minutes)
     * and expect the maximum number of open channels to be a two digit number.
     */
    size_t dropIdleChannels(Minutes sinceLastUsed) {
        auto keepIf = [threshold = _clockSource->now() - sinceLastUsed](const auto& cs) {
            if (cs.use_count() > 1 || cs->lastUsed() > threshold)
                // There are stubs referencing this channel, or it's recently used.
                return true;
            return false;
        };
        auto droppedChannels = _dropChannels(std::move(keepIf));

        for (const auto& channel : droppedChannels) {
            LOGV2_INFO(7401802,
                       "Dropping idle gRPC channel",
                       "remote"_attr = channel->remote(),
                       "useSSL"_attr = channel->useSSL(),
                       "lastUsed"_attr = channel->lastUsed());
        }
        return droppedChannels.size();
    }

    /**
     * Drops all channels and returns the number of dropped channels. May only be called when all
     * stub handles (i.e., instances of `StubHandle`) created by this pool are released. Otherwise,
     * it will terminate the process.
     */
    size_t dropAllChannels() {
        auto droppedChannels = _dropChannels([](const auto&) { return false; });
        for (const auto& channel : droppedChannels) {
            LOGV2_INFO(7401803,
                       "Dropping gRPC channel as part of dropping all channels",
                       "remote"_attr = channel->remote(),
                       "useSSL"_attr = channel->useSSL());
        }
        return droppedChannels.size();
    }

    size_t size() const {
        auto lk = stdx::lock_guard(_mutex);
        return _channels.size();
    }

private:
    /**
     * Iterates through all channels, calls into `shouldKeep` for each channel with a reference to
     * its `ChannelState`, and decides if the channel should be dropped based on the return value.
     * A channel cannot be dropped so long as it's being referenced by a `Stub`. Attempting to do
     * so is a process fatal event.
     * Returns a vector containing the only reference to the dropped channels.
     */
    std::vector<std::shared_ptr<ChannelState>> _dropChannels(
        std::function<bool(const std::shared_ptr<ChannelState>&)> shouldKeep) {
        std::vector<std::shared_ptr<ChannelState>> droppedChannels;
        auto lk = stdx::lock_guard(_mutex);
        for (auto iter = _channels.begin(); iter != _channels.end();) {
            auto prev = iter++;
            const auto& cs = prev->second;
            if (shouldKeep(cs))
                continue;
            invariant(cs.use_count() == 1, "Attempted to drop a channel with existing stubs");
            droppedChannels.push_back(std::move(prev->second));
            _channels.erase(prev);
        }
        return droppedChannels;
    }

    ClockSource* const _clockSource;
    SSLModeResolver _sslModeResolver;
    ChannelFactory _channelFactory;
    StubFactory _stubFactory;

    mutable stdx::mutex _mutex;  // NOLINT

    using ChannelMapKeyType = std::pair<HostAndPort, bool>;
    stdx::unordered_map<ChannelMapKeyType, std::shared_ptr<ChannelState>> _channels;
};

}  // namespace mongo::transport::grpc

#undef MONGO_LOGV2_DEFAULT_COMPONENT
