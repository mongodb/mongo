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

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/service_liaison_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/periodic_runner_factory.h"

namespace mongo {

MockServiceLiaisonImpl::MockServiceLiaisonImpl() {
    _timerFactory = stdx::make_unique<executor::AsyncTimerFactoryMock>();
    _runner = makePeriodicRunner(getGlobalServiceContext());
    _runner->startup();
}

LogicalSessionIdSet MockServiceLiaisonImpl::getActiveOpSessions() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _activeSessions;
}

LogicalSessionIdSet MockServiceLiaisonImpl::getOpenCursorSessions() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _cursorSessions;
}

void MockServiceLiaisonImpl::join() {
    _runner->shutdown();
}

Date_t MockServiceLiaisonImpl::now() const {
    return _timerFactory->now();
}

void MockServiceLiaisonImpl::scheduleJob(PeriodicRunner::PeriodicJob job) {
    // The cache should be refreshed from tests by calling refreshNow().
    return;
}


void MockServiceLiaisonImpl::addCursorSession(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cursorSessions.insert(std::move(lsid));
}

void MockServiceLiaisonImpl::removeCursorSession(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cursorSessions.erase(lsid);
}

void MockServiceLiaisonImpl::clearCursorSession() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cursorSessions.clear();
}

void MockServiceLiaisonImpl::add(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cursorSessions.insert(std::move(lsid));
}

void MockServiceLiaisonImpl::remove(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _activeSessions.erase(lsid);
}

void MockServiceLiaisonImpl::clear() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

std::pair<Status, int> MockServiceLiaisonImpl::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {

    _matcher = matcher;
    return std::make_pair(Status::OK(), 0);
}

}  // namespace mongo
