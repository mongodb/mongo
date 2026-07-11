// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/service_liaison_mock.h"

#include "mongo/util/periodic_runner_factory.h"

#include <memory>
#include <mutex>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MockServiceLiaisonImpl::MockServiceLiaisonImpl() {
    _timerFactory = std::make_unique<executor::AsyncTimerFactoryMock>();
    _runner = makePeriodicRunner(getGlobalServiceContext());
}

LogicalSessionIdSet MockServiceLiaisonImpl::getActiveOpSessions() const {
    std::unique_lock<std::mutex> lk(_mutex);
    return _activeSessions;
}

LogicalSessionIdSet MockServiceLiaisonImpl::getOpenCursorSessions(OperationContext* opCtx) const {
    std::unique_lock<std::mutex> lk(_mutex);
    return _cursorSessions;
}

void MockServiceLiaisonImpl::join() {}

Date_t MockServiceLiaisonImpl::now() const {
    return _timerFactory->now();
}

void MockServiceLiaisonImpl::scheduleJob(PeriodicRunner::PeriodicJob job) {
    // The cache should be refreshed from tests by calling refreshNow().
    return;
}


void MockServiceLiaisonImpl::addCursorSession(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    _cursorSessions.insert(std::move(lsid));
}

void MockServiceLiaisonImpl::removeCursorSession(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    _cursorSessions.erase(lsid);
}

void MockServiceLiaisonImpl::clearCursorSession() {
    std::unique_lock<std::mutex> lk(_mutex);
    _cursorSessions.clear();
}

void MockServiceLiaisonImpl::add(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    _cursorSessions.insert(std::move(lsid));
}

void MockServiceLiaisonImpl::remove(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    _activeSessions.erase(lsid);
}

void MockServiceLiaisonImpl::clear() {
    std::unique_lock<std::mutex> lk(_mutex);
    _activeSessions.clear();
}

void MockServiceLiaisonImpl::fastForward(Milliseconds time) {
    _timerFactory->fastForward(time);
}

int MockServiceLiaisonImpl::jobs() {
    return _timerFactory->jobs();
}

const KillAllSessionsByPattern* MockServiceLiaisonImpl::matchKilled(const LogicalSessionId& lsid) {
    return _matcher->match(lsid);
}

int MockServiceLiaisonImpl::killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                            const SessionKiller::Matcher& matcher) {

    _matcher = matcher;
    return 0;
}

}  // namespace mongo
