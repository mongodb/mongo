// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <set>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace executor {
namespace connection_pool_test_details {

class ConnectionPoolTest;

class PoolImpl;

/**
 * Mock interface for the timer
 */
class TimerImpl final : public ConnectionPool::TimerInterface {
public:
    explicit TimerImpl(PoolImpl* global);
    ~TimerImpl() override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;

    void cancelTimeout() override;

    Date_t now() override;

    // launches all timers for whom now() has passed
    static void fireIfNecessary();

    // dump all timers
    static void clear();

private:
    static std::set<TimerImpl*> _timers;

    TimeoutCallback _cb;
    PoolImpl* _global;
    Date_t _expiration;
};

/**
 * Mock interface for the connections
 *
 * pushSetup() and pushRefresh() calls can be queued up ahead of time (in which
 * case callbacks immediately fire), or calls queue up and pushSetup() and
 * pushRefresh() fire as they're called.
 */
class ConnectionImpl final : public ConnectionPool::ConnectionInterface {
public:
    using PushSetupCallback = unique_function<Status()>;
    using PushRefreshCallback = unique_function<Status()>;

    ConnectionImpl(const HostAndPort& hostAndPort,
                   PoolConnectionId,
                   size_t generation,
                   PoolImpl* global);

    size_t id() const;

    const HostAndPort& getHostAndPort() const override;
    transport::ConnectSSLMode getSslMode() const override {
        return transport::kGlobalSSLMode;
    }

    bool isHealthy() override;

    void setUnhealthy();

    // Dump all connection callbacks
    static void clear();

    // Push either a callback that returns the status for a setup, or just the Status
    static void pushSetup(PushSetupCallback status);
    static void pushSetup(Status status);
    static size_t setupQueueDepth();

    // Push either a callback that returns the status for a refresh, or just the Status
    static void pushRefresh(PushRefreshCallback status);
    static void pushRefresh(Status status);
    static size_t refreshQueueDepth();

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;

    void cancelTimeout() override;

    Date_t now() override;

private:
    void setup(Milliseconds timeout, SetupCallback cb, std::string) override;

    void refresh(Milliseconds timeout, RefreshCallback cb) override;

    static void processSetup();
    static void processRefresh();

    HostAndPort _hostAndPort;
    bool _healthy{true};
    SetupCallback _setupCallback;
    RefreshCallback _refreshCallback;
    TimerImpl _timer;
    PoolImpl* _global;
    size_t _id;

    // Answer queues
    static std::deque<PushSetupCallback> _pushSetupQueue;
    static std::deque<PushRefreshCallback> _pushRefreshQueue;

    // Question queues
    static std::deque<ConnectionImpl*> _setupQueue;
    static std::deque<ConnectionImpl*> _refreshQueue;

    static size_t _idCounter;
};

/**
 * Mock for the pool implementation
 */
class PoolImpl final : public ConnectionPool::DependentTypeFactoryInterface {
    friend class ConnectionImpl;
    friend class TimerImpl;

public:
    explicit PoolImpl(const std::shared_ptr<OutOfLineExecutor>& executor) : _executor(executor) {}
    std::shared_ptr<ConnectionPool::ConnectionInterface> makeConnection(
        const HostAndPort& hostAndPort,
        transport::ConnectSSLMode sslMode,
        PoolConnectionId,
        size_t generation) override;

    std::shared_ptr<ConnectionPool::TimerInterface> makeTimer() override;

    const std::shared_ptr<OutOfLineExecutor>& getExecutor() override;

    Date_t now() override;

    ClockSource* getFastClockSource() override {
        return &_fastClockSource;
    }

    void shutdown() override {
        TimerImpl::clear();
    };

    /**
     * setNow() can be used to fire all timers that have passed a point in time
     */
    static void setNow(Date_t now);

private:
    ConnectionPool* _pool = nullptr;
    std::shared_ptr<OutOfLineExecutor> _executor;

    static boost::optional<Date_t> _now;
    static ClockSourceMock _fastClockSource;
};

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
