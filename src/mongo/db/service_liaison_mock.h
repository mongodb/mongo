// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
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

    mutable std::mutex _mutex;
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
