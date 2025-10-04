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
#include "mongo/crypto/hash_block.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/service_liaison.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * To allow us to move a MockServiceLiaison into the session cache while
 * maintaining a hold on it from within our unit tests, the MockServiceLiaison
 * will have an internal pointer to a MockServiceLiaisonImpl object that the
 * test creates and controls.
 *
 * This class maintains an internal _activeSessions list that may be modified
 * by the test caller. It also maintains an internal mocked representation of
 * time, which the caller can fastForward(). The Date_t returned by now() will
 * be the epoch + the amount of minutes this object has been fast-forwarded over
 * course of its life.
 *
 * This service liaison starts up its internal periodic runner on construction.
 */
class MockServiceLiaisonImpl {
public:
    MockServiceLiaisonImpl();

    // Forwarding methods from the MockServiceLiaison
    LogicalSessionIdSet getActiveOpSessions() const;
    LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) const;
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
    int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher);

private:
    std::unique_ptr<executor::AsyncTimerFactoryMock> _timerFactory;
    std::unique_ptr<PeriodicRunner> _runner;

    boost::optional<SessionKiller::Matcher> _matcher;

    mutable stdx::mutex _mutex;
    LogicalSessionIdSet _activeSessions;
    LogicalSessionIdSet _cursorSessions;
};

/**
 * A mock service liaison for testing the logical session cache.
 */
class MockServiceLiaison : public ServiceLiaison {
public:
    explicit MockServiceLiaison(std::shared_ptr<MockServiceLiaisonImpl> impl)
        : _impl(std::move(impl)) {}

    LogicalSessionIdSet getActiveOpSessions() const override {
        return _impl->getActiveOpSessions();
    }

    LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) const override {
        return _impl->getOpenCursorSessions(opCtx);
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

    int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher) override {
        return _impl->killCursorsWithMatchingSessions(opCtx, matcher);
    }

protected:
    ServiceContext* _context() override {
        return _serviceContext.get();
    }

private:
    std::shared_ptr<MockServiceLiaisonImpl> _impl;
    ServiceContext::UniqueServiceContext _serviceContext;
};

}  // namespace mongo
