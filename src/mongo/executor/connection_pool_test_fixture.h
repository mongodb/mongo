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
