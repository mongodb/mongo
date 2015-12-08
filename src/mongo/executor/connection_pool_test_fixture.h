/** *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <deque>
#include <memory>
#include <set>

#include "mongo/executor/connection_pool.h"

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
    TimerImpl(PoolImpl* global);
    ~TimerImpl() override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;

    void cancelTimeout() override;

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
    using PushSetupCallback = stdx::function<Status()>;
    using PushRefreshCallback = stdx::function<Status()>;

    ConnectionImpl(const HostAndPort& hostAndPort, size_t generation, PoolImpl* global);

    size_t id() const;

    void indicateSuccess() override;
    void indicateFailure(Status status) override;

    void resetToUnknown() override;

    const HostAndPort& getHostAndPort() const override;

    bool isHealthy() override;

    // Dump all connection callbacks
    static void clear();

    // Push either a callback that returns the status for a setup, or just the Status
    static void pushSetup(PushSetupCallback status);
    static void pushSetup(Status status);

    // Push either a callback that returns the status for a refresh, or just the Status
    static void pushRefresh(PushRefreshCallback status);
    static void pushRefresh(Status status);

private:
    void indicateUsed() override;

    Date_t getLastUsed() const override;

    const Status& getStatus() const override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;

    void cancelTimeout() override;

    void setup(Milliseconds timeout, SetupCallback cb) override;

    void refresh(Milliseconds timeout, RefreshCallback cb) override;

    size_t getGeneration() const override;

    HostAndPort _hostAndPort;
    Date_t _lastUsed;
    Status _status = Status::OK();
    SetupCallback _setupCallback;
    RefreshCallback _refreshCallback;
    TimerImpl _timer;
    PoolImpl* _global;
    size_t _id;
    size_t _generation;

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

public:
    std::unique_ptr<ConnectionPool::ConnectionInterface> makeConnection(
        const HostAndPort& hostAndPort, size_t generation) override;

    std::unique_ptr<ConnectionPool::TimerInterface> makeTimer() override;

    Date_t now() override;

    /**
     * setNow() can be used to fire all timers that have passed a point in time
     */
    static void setNow(Date_t now);

private:
    ConnectionPool* _pool = nullptr;

    static boost::optional<Date_t> _now;
};

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
