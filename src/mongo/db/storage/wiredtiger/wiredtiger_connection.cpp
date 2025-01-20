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

#include <cerrno>
#include <cstdlib>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <wiredtiger.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const size_t numRegistryPartitions = 2 * ProcessInfo::getNumLogicalCores();

}  // namespace

WiredTigerConnection::WiredTigerConnection(WiredTigerKVEngine* engine)
    : WiredTigerConnection(engine->getConn(), engine->getClockSource(), engine) {}

WiredTigerConnection::WiredTigerConnection(WT_CONNECTION* conn,
                                           ClockSource* cs,
                                           WiredTigerKVEngine* engine)
    : _conn(conn), _clockSource(cs), _engine(engine), _registry(numRegistryPartitions) {
    uassertStatusOK(_compiledConfigurations.compileAll(_conn));
}

WiredTigerConnection::~WiredTigerConnection() {
    shuttingDown();
}

void WiredTigerConnection::shuttingDown() {
    // Try to atomically set _shuttingDown flag, but just return if another thread was first.
    if (_shuttingDown.fetchAndBitOr(kShuttingDownMask) & kShuttingDownMask)
        return;

    // Spin as long as there are threads blocking shutdown.
    while (_shuttingDown.load() != kShuttingDownMask) {
        sleepmillis(1);
    }

    closeAll();
}

void WiredTigerConnection::restart() {
    _shuttingDown.fetchAndBitAnd(~kShuttingDownMask);
}

bool WiredTigerConnection::isShuttingDown() {
    return _shuttingDown.load() & kShuttingDownMask;
}

void WiredTigerConnection::waitUntilPreparedUnitOfWorkCommitsOrAborts(Interruptible& interruptible,
                                                                      std::uint64_t lastCount) {
    // It is possible for a prepared transaction to block on bonus eviction inside WiredTiger after
    // it commits or rolls-back, but this delays it from signalling us to wake up. In the very
    // worst case that the only evictable page is the one pinned by our cursor, AND there are no
    // other prepared transactions committing or aborting, we could reach a deadlock. Since the
    // caller is already expecting spurious wakeups, we impose a large timeout to periodically force
    // the caller to retry its operation.
    const auto deadline = Date_t::now() + Seconds(1);
    stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);
    if (lastCount == _prepareCommitOrAbortCounter.loadRelaxed()) {
        interruptible.waitForConditionOrInterruptUntil(
            _prepareCommittedOrAbortedCond, lk, deadline, [&] {
                return _prepareCommitOrAbortCounter.loadRelaxed() > lastCount;
            });
    }
}

void WiredTigerConnection::notifyPreparedUnitOfWorkHasCommittedOrAborted() {
    stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);
    _prepareCommitOrAbortCounter.fetchAndAdd(1);
    _prepareCommittedOrAbortedCond.notify_all();
}


void WiredTigerConnection::closeAllCursors(const std::string& uri) {
    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    for (SessionCache::iterator i = _sessions.begin(); i != _sessions.end(); i++) {
        (*i)->closeAllCursors(uri);
    }
}

size_t WiredTigerConnection::getIdleSessionsCount() {
    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    return _sessions.size();
}

void WiredTigerConnection::closeExpiredIdleSessions(int64_t idleTimeMillis) {
    // Do nothing if session close idle time is set to 0 or less
    if (idleTimeMillis <= 0) {
        return;
    }

    auto cutoffTime = _clockSource->now() - Milliseconds(idleTimeMillis);
    SessionCache sessionsToClose;

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        // Discard all sessions that became idle before the cutoff time
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            auto session = *it;
            invariant(session->getIdleExpireTime() != Date_t::min());
            if (session->getIdleExpireTime() < cutoffTime) {
                it = _sessions.erase(it);
                sessionsToClose.push_back(session);
            } else {
                ++it;
            }
        }
    }

    // Closing expired idle sessions is expensive, so do it outside of the cache mutex. This helps
    // to avoid periodic operation latency spikes as seen in SERVER-52879.
    for (auto session : sessionsToClose) {
        delete session;
    }
}

void WiredTigerConnection::closeAll() {
    // Increment the epoch as we are now closing all sessions with this epoch.
    SessionCache swap;

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        _epoch.fetchAndAdd(1);
        _sessions.swap(swap);
    }

    for (SessionCache::iterator i = swap.begin(); i != swap.end(); i++) {
        delete (*i);
    }
}

bool WiredTigerConnection::isEphemeral() {
    return _engine && _engine->isEphemeral();
}

UniqueWiredTigerSession WiredTigerConnection::getSession() {
    // We should never be able to get here after _shuttingDown is set, because no new
    // operations should be allowed to start.
    invariant(!(_shuttingDown.load() & kShuttingDownMask));

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        if (!_sessions.empty()) {
            // Get the most recently used session so that if we discard sessions, we're
            // discarding older ones
            WiredTigerSession* cachedSession = _sessions.back();
            _sessions.pop_back();
            // Reset the idle time
            cachedSession->setIdleExpireTime(Date_t::min());
            return UniqueWiredTigerSession(cachedSession);
        }
    }

    // Outside of the cache partition lock, but on release will be put back on the cache
    return UniqueWiredTigerSession(new WiredTigerSession(this, _epoch.load()));
}

void WiredTigerConnection::_releaseSession(WiredTigerSession* session) {
    invariant(session);

    BlockShutdown blockShutdown(this);

    uint64_t currentEpoch = _epoch.load();
    if (isShuttingDown() || session->_getEpoch() != currentEpoch) {
        invariant(session->_getEpoch() <= currentEpoch);
        // There is a race condition with clean shutdown, where the storage engine is ripped from
        // underneath OperationContexts, which are not "active" (i.e., do not have any locks), but
        // are just about to delete the recovery unit. See SERVER-16031 for more information. Since
        // shutting down the WT_CONNECTION will close all WT_SESSIONS, we shouldn't also try to
        // directly close this session.
        session->dropSessionBeforeDeleting();
        delete session;
        return;
    }

    invariant(session->cursorsOut() == 0);

    {
        WT_SESSION* ss = session->getSession();
        uint64_t range;
        // This checks that we are only caching idle sessions and not something which might hold
        // locks or otherwise prevent truncation.
        invariantWTOK(ss->transaction_pinned_range(ss, &range), ss);
        invariant(range == 0);

        // Release resources in the session we're about to cache.
        // If we are using hybrid caching, then close cursors now and let them
        // be cached at the WiredTiger level.
        if (gWiredTigerCursorCacheSize.load() < 0) {
            session->closeAllCursors("");
        }

        session->resetSessionConfiguration();
        invariantWTOK(ss->reset(ss), ss);
    }

    // Set the time this session got idle at.
    session->setIdleExpireTime(_clockSource->now());
    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        _sessions.push_back(session);
    }

    if (_engine) {
        _engine->sizeStorerPeriodicFlush();
    }
}

bool WiredTigerConnection::isEngineCachingCursors() {
    return gWiredTigerCursorCacheSize.load() <= 0;
}

void WiredTigerConnection::WiredTigerSessionDeleter::operator()(WiredTigerSession* session) const {
    session->_connection->_releaseSession(session);
}

WiredTigerSession* WiredTigerConnection::getSessionById(const SessionId& id) {
    RegistryPartition& partition = _registry[id % numRegistryPartitions];
    stdx::lock_guard<stdx::mutex> lock(partition.mtx);
    return partition.map[id];
}

void WiredTigerConnection::_addSession(const SessionId& id, WiredTigerSession* session) {
    RegistryPartition& partition = _registry[id % numRegistryPartitions];
    stdx::lock_guard<stdx::mutex> lock(partition.mtx);
    invariant(partition.map.insert({id, session}).second);
}

bool WiredTigerConnection::_removeSession(const SessionId& id) {
    RegistryPartition& partition = _registry[id % numRegistryPartitions];
    stdx::lock_guard<stdx::mutex> lock(partition.mtx);
    return partition.map.erase(id);
}

WT_SESSION* WiredTigerConnection::_openSession(WiredTigerSession* session,
                                               WT_EVENT_HANDLER* handler,
                                               const char* config) {
    return _openSessionInternal(session, handler, config, _conn);
}

WT_SESSION* WiredTigerConnection::_openSession(WiredTigerSession* session,
                                               StatsCollectionPermit& permit,
                                               const char* config) {
    invariant(permit.conn());
    return _openSessionInternal(session, nullptr, config, permit.conn());
}

WT_SESSION* WiredTigerConnection::_openSessionInternal(WiredTigerSession* session,
                                                       WT_EVENT_HANDLER* handler,
                                                       const char* config,
                                                       WT_CONNECTION* conn) {
    WT_SESSION* rawSession;
    invariantWTOK(conn->open_session(conn, handler, config, &rawSession), nullptr);
    // TODO(SERVER-99353): _addSession(id, session).
    return rawSession;
}

}  // namespace mongo
