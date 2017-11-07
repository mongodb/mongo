/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/service_liason.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/periodic_runner_asio.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * To allow us to move a MockServiceLiason into the session cache while
 * maintaining a hold on it from within our unit tests, the MockServiceLiason
 * will have an internal pointer to a MockServiceLiasonImpl object that the
 * test creates and controls.
 *
 * This class maintains an internal _activeSessions list that may be modified
 * by the test caller. It also maintains an internal mocked representation of
 * time, which the caller can fastForward(). The Date_t returned by now() will
 * be the epoch + the amount of minutes this object has been fast-forwarded over
 * course of its life.
 *
 * This service liason starts up its internal periodic runner on construction.
 */
class MockServiceLiasonImpl {
public:
    MockServiceLiasonImpl();

    // Forwarding methods from the MockServiceLiason
    LogicalSessionIdSet getActiveOpSessions() const;
    LogicalSessionIdSet getOpenCursorSessions() const;
    Date_t now() const;
    void scheduleJob(PeriodicRunner::PeriodicJob job);
    void join();

    // Test-side methods that operate on the _activeSessions list.
    void add(LogicalSessionId lsid);
    void remove(LogicalSessionId lsid);
    void clear();

    void addCursorSession(LogicalSessionId lsid);
    void removeCursorSession(LogicalSessionId lsid);
    void clearCursorSession();

    void fastForward(Milliseconds time);
    int jobs();

    const KillAllSessionsByPattern* matchKilled(const LogicalSessionId& lsid);
    std::pair<Status, int> killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                           const SessionKiller::Matcher& matcher);

private:
    executor::AsyncTimerFactoryMock* _timerFactory;
    std::unique_ptr<PeriodicRunnerASIO> _runner;

    boost::optional<SessionKiller::Matcher> _matcher;

    mutable stdx::mutex _mutex;
    LogicalSessionIdSet _activeSessions;
    LogicalSessionIdSet _cursorSessions;
};

/**
 * A mock service liason for testing the logical session cache.
 */
class MockServiceLiason : public ServiceLiason {
public:
    explicit MockServiceLiason(std::shared_ptr<MockServiceLiasonImpl> impl)
        : _impl(std::move(impl)) {}

    LogicalSessionIdSet getActiveOpSessions() const override {
        return _impl->getActiveOpSessions();
    }

    LogicalSessionIdSet getOpenCursorSessions() const override {
        return _impl->getOpenCursorSessions();
    }

    Date_t now() const override {
        return _impl->now();
    }

    void scheduleJob(PeriodicRunner::PeriodicJob job) override {
        _impl->scheduleJob(std::move(job));
    }

    void join() override {
        return _impl->join();
    }

    std::pair<Status, int> killCursorsWithMatchingSessions(
        OperationContext* opCtx, const SessionKiller::Matcher& matcher) override {
        return _impl->killCursorsWithMatchingSessions(opCtx, matcher);
    }

protected:
    ServiceContext* _context() override {
        return _serviceContext.get();
    }

private:
    std::shared_ptr<MockServiceLiasonImpl> _impl;
    std::unique_ptr<ServiceContextNoop> _serviceContext;
};

}  // namespace mongo
