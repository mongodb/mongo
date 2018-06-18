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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <queue>
#include <sqlite3.h>
#include <string>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/db/storage/mobile/mobile_session_pool.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"

namespace mongo {

MobileDelayedOpQueue::MobileDelayedOpQueue() : _isEmpty(true) {}

void MobileDelayedOpQueue::enqueueOp(std::string& opQuery) {
    _queueMutex.lock();
    // If the queue is empty till now, update the cached atomic to reflect the new state.
    if (_opQueryQueue.empty())
        _isEmpty.store(false);
    _opQueryQueue.push(opQuery);
    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Enqueued operation for delayed execution: " << opQuery;
    _queueMutex.unlock();
}

void MobileDelayedOpQueue::execAndDequeueOp(MobileSession* session) {
    std::string opQuery;

    _queueMutex.lock();
    if (!_opQueryQueue.empty()) {
        opQuery = _opQueryQueue.front();
        _opQueryQueue.pop();
        // If the queue is empty now, set the cached atomic to reflect the new state.
        if (_opQueryQueue.empty())
            _isEmpty.store(true);
    }
    _queueMutex.unlock();

    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Retrying previously enqueued operation: " << opQuery;
    try {
        SqliteStatement::execQuery(session, opQuery);
    } catch (const WriteConflictException&) {
        // It is possible that this operation fails because of a transaction running in parallel.
        // We re-enqueue it for now and keep retrying later.
        LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Caught WriteConflictException while executing "
                                     " previously enqueued operation, re-enquing it";
        enqueueOp(opQuery);
    }
}

void MobileDelayedOpQueue::execAndDequeueAllOps(MobileSession* session) {
    // Keep trying till the queue empties
    while (!_isEmpty.load())
        execAndDequeueOp(session);
}

bool MobileDelayedOpQueue::isEmpty() {
    return (_isEmpty.load());
}

MobileSessionPool::MobileSessionPool(const std::string& path, std::uint64_t maxPoolSize)
    : _path(path), _maxPoolSize(maxPoolSize) {}

MobileSessionPool::~MobileSessionPool() {
    shutDown();
}

std::unique_ptr<MobileSession> MobileSessionPool::getSession(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // We should never be able to get here after _shuttingDown is set, because no new operations
    // should be allowed to start.
    invariant(!_shuttingDown);

    // Checks if there is an open session available.
    if (!_sessions.empty()) {
        sqlite3* session = _popSession_inlock();
        return stdx::make_unique<MobileSession>(session, this);
    }

    // Checks if a new session can be opened.
    if (_curPoolSize < _maxPoolSize) {
        sqlite3* session;
        int status = sqlite3_open(_path.c_str(), &session);
        checkStatus(status, SQLITE_OK, "sqlite3_open");
        _curPoolSize++;
        return stdx::make_unique<MobileSession>(session, this);
    }

    // There are no open sessions available and the maxPoolSize has been reached.
    // waitForConditionOrInterrupt is notified when an open session is released and available.
    opCtx->waitForConditionOrInterrupt(
        _releasedSessionNotifier, lk, [&] { return !_sessions.empty(); });

    sqlite3* session = _popSession_inlock();
    return stdx::make_unique<MobileSession>(session, this);
}

void MobileSessionPool::releaseSession(MobileSession* session) {
    // Retry drop that have been queued on failure
    if (!failedDropsQueue.isEmpty())
        failedDropsQueue.execAndDequeueOp(session);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _sessions.push_back(session->getSession());
    _releasedSessionNotifier.notify_one();
}

void MobileSessionPool::shutDown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _shuttingDown = true;

    // Retrieve the operation context from the thread's client if the client exists.
    if (haveClient()) {
        OperationContext* opCtx = cc().getOperationContext();

        // Locks if the operation context still exists.
        if (opCtx) {
            opCtx->waitForConditionOrInterrupt(
                _releasedSessionNotifier, lk, [&] { return _sessions.size() == _curPoolSize; });
        }
    } else {
        _releasedSessionNotifier.wait(lk, [&] { return _sessions.size() == _curPoolSize; });
    }

    // Retry all the drops that have been queued on failure.
    // Create a new sqlite session to do so, all other sessions might have been closed already.
    if (!failedDropsQueue.isEmpty()) {
        sqlite3* session;

        int status = sqlite3_open(_path.c_str(), &session);
        checkStatus(status, SQLITE_OK, "sqlite3_open");
        std::unique_ptr<MobileSession> mobSession = stdx::make_unique<MobileSession>(session, this);
        LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Executing queued drops at shutdown";
        failedDropsQueue.execAndDequeueAllOps(mobSession.get());
        sqlite3_close(session);
    }

    for (auto&& session : _sessions) {
        sqlite3_close(session);
    }
}

// This method should only be called when _sessions is locked.
sqlite3* MobileSessionPool::_popSession_inlock() {
    sqlite3* session = _sessions.back();
    _sessions.pop_back();
    return session;
}

}  // namespace mongo
