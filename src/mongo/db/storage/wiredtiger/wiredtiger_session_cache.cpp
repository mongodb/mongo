// wiredtiger_session_cache.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

// The "wiredTigerCursorCacheSize" parameter has the following meaning.
//
// wiredTigerCursorCacheSize == 0
// For this setting, cursors are only cached in the WiredTiger storage engine
// itself. Operations that need exclusive access such as drop or verify will
// not be blocked by inactive cached cursors with this setting. However, this
// setting may reduce the performance of certain workloads that normally
// benefit from cursor caching above the storage engine.
//
// wiredTigerCursorCacheSize > 0
// WiredTiger-level caching of cursors is disabled but cursor caching does
// occur above the storage engine. The value of this setting represents the
// maximum number of cursors that are cached. Setting the value to 10000 will
// give the old (<= 3.6) behavior. Note that cursors remain cached, even when a
// session is released back to the cache. Thus, exclusive operations may be
// blocked temporarily, and in some cases, a long time. Drops that fail because
// of exclusivity silently succeed and are queued for retries.
//
// wiredTigerCursorCacheSize < 0
// This is a hybrid approach of the above two, and is the default. The the
// absolute value of the setting is used as the number of cursors cached above
// the storage engine. When a session is released, all cursors are closed, and
// will be cached in WiredTiger. Exclusive operations should only be blocked
// for a short time, except if a cursor is held by a long running session. This
// is a good compromise for most workloads.
AtomicInt32 kWiredTigerCursorCacheSize(-100);

const std::string kWTRepairMsg =
    "Please read the documentation for starting MongoDB with --repair here: "
    "http://dochub.mongodb.org/core/repair";

ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>
    WiredTigerCursorCacheSizeSetting(ServerParameterSet::getGlobal(),
                                     "wiredTigerCursorCacheSize",
                                     &kWiredTigerCursorCacheSize);

WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn, uint64_t epoch, uint64_t cursorEpoch)
    : _epoch(epoch), _cursorEpoch(cursorEpoch), _session(NULL), _cursorGen(0), _cursorsOut(0) {
    invariantWTOK(conn->open_session(conn, NULL, "isolation=snapshot", &_session));
}

WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn,
                                     WiredTigerSessionCache* cache,
                                     uint64_t epoch,
                                     uint64_t cursorEpoch)
    : _epoch(epoch),
      _cursorEpoch(cursorEpoch),
      _cache(cache),
      _session(NULL),
      _cursorGen(0),
      _cursorsOut(0) {
    invariantWTOK(conn->open_session(conn, NULL, "isolation=snapshot", &_session));
}

WiredTigerSession::~WiredTigerSession() {
    if (_session) {
        invariantWTOK(_session->close(_session, NULL));
    }
}

WT_CURSOR* WiredTigerSession::getCursor(const std::string& uri, uint64_t id, bool forRecordStore) {
    // Find the most recently used cursor
    for (CursorCache::iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        if (i->_id == id) {
            WT_CURSOR* c = i->_cursor;
            _cursors.erase(i);
            _cursorsOut++;
            return c;
        }
    }

    WT_CURSOR* c = NULL;
    int ret = _session->open_cursor(
        _session, uri.c_str(), NULL, forRecordStore ? "" : "overwrite=false", &c);
    if (ret == EBUSY) {
        // This can only happen when trying to open a cursor on the oplog and it is currently locked
        // by a verify or salvage, because we don't employ database locks to protect the oplog.
        throw WriteConflictException();
    }
    if (ret != 0) {
        error() << "Failed to open a WiredTiger cursor: " << uri;
        error() << "This may be due to data corruption. " << kWTRepairMsg;

        fassertFailedNoTrace(50882);
    }
    _cursorsOut++;
    return c;
}

void WiredTigerSession::releaseCursor(uint64_t id, WT_CURSOR* cursor) {
    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

    invariantWTOK(cursor->reset(cursor));

    // Cursors are pushed to the front of the list and removed from the back
    _cursors.push_front(WiredTigerCachedCursor(id, _cursorGen++, cursor));

    // A negative value for wiredTigercursorCacheSize means to use hybrid caching.
    std::uint32_t cacheSize = abs(kWiredTigerCursorCacheSize.load());

    while (!_cursors.empty() && _cursorGen - _cursors.back()._gen > cacheSize) {
        cursor = _cursors.back()._cursor;
        _cursors.pop_back();
        invariantWTOK(cursor->close(cursor));
    }
}

void WiredTigerSession::closeAllCursors(const std::string& uri) {
    invariant(_session);

    bool all = (uri == "");
    for (auto i = _cursors.begin(); i != _cursors.end();) {
        WT_CURSOR* cursor = i->_cursor;
        if (cursor && (all || uri == cursor->uri)) {
            invariantWTOK(cursor->close(cursor));
            i = _cursors.erase(i);
        } else
            ++i;
    }
}

void WiredTigerSession::closeCursorsForQueuedDrops(WiredTigerKVEngine* engine) {
    invariant(_session);

    _cursorEpoch = _cache->getCursorEpoch();
    auto toDrop = engine->filterCursorsWithQueuedDrops(&_cursors);

    for (auto i = toDrop.begin(); i != toDrop.end(); i++) {
        WT_CURSOR* cursor = i->_cursor;
        if (cursor) {
            invariantWTOK(cursor->close(cursor));
        }
    }
}

namespace {
AtomicUInt64 nextTableId(1);
}
// static
uint64_t WiredTigerSession::genTableId() {
    return nextTableId.fetchAndAdd(1);
}

// -----------------------

WiredTigerSessionCache::WiredTigerSessionCache(WiredTigerKVEngine* engine)
    : _engine(engine), _conn(engine->getConnection()), _shuttingDown(0) {}

WiredTigerSessionCache::WiredTigerSessionCache(WT_CONNECTION* conn)
    : _engine(NULL), _conn(conn), _shuttingDown(0) {}

WiredTigerSessionCache::~WiredTigerSessionCache() {
    shuttingDown();
}

void WiredTigerSessionCache::shuttingDown() {
    uint32_t actual = _shuttingDown.load();
    uint32_t expected;

    // Try to atomically set _shuttingDown flag, but just return if another thread was first.
    do {
        expected = actual;
        actual = _shuttingDown.compareAndSwap(expected, expected | kShuttingDownMask);
        if (actual & kShuttingDownMask)
            return;
    } while (actual != expected);

    // Spin as long as there are threads in releaseSession
    while (_shuttingDown.load() != kShuttingDownMask) {
        sleepmillis(1);
    }

    closeAll();
}

void WiredTigerSessionCache::waitUntilDurable(bool forceCheckpoint, bool stableCheckpoint) {
    // For inMemory storage engines, the data is "as durable as it's going to get".
    // That is, a restart is equivalent to a complete node failure.
    if (isEphemeral()) {
        return;
    }

    const int shuttingDown = _shuttingDown.fetchAndAdd(1);
    ON_BLOCK_EXIT([this] { _shuttingDown.fetchAndSubtract(1); });

    uassert(ErrorCodes::ShutdownInProgress,
            "Cannot wait for durability because a shutdown is in progress",
            !(shuttingDown & kShuttingDownMask));

    // Stable checkpoints are only meaningful in a replica set. Replication sets the "stable
    // timestamp". If the stable timestamp is unset, WiredTiger takes a full checkpoint, which is
    // incidentally what we want. A "true" stable checkpoint (a stable timestamp was set on the
    // WT_CONNECTION, i.e: replication is on) requires `forceCheckpoint` to be true and journaling
    // to be enabled.
    if (stableCheckpoint && getGlobalReplSettings().usingReplSets()) {
        invariant(forceCheckpoint && _engine->isDurable());
    }

    // When forcing a checkpoint with journaling enabled, don't synchronize with other
    // waiters, as a log flush is much cheaper than a full checkpoint.
    if (forceCheckpoint && _engine->isDurable()) {
        UniqueWiredTigerSession session = getSession();
        WT_SESSION* s = session->getSession();
        {
            stdx::unique_lock<stdx::mutex> lk(_journalListenerMutex);
            JournalListener::Token token = _journalListener->getToken();
            auto config = stableCheckpoint ? "use_timestamp=true" : "use_timestamp=false";
            invariantWTOK(s->checkpoint(s, config));
            _journalListener->onDurable(token);
        }
        LOG(4) << "created checkpoint (forced)";
        return;
    }

    uint32_t start = _lastSyncTime.load();
    // Do the remainder in a critical section that ensures only a single thread at a time
    // will attempt to synchronize.
    stdx::unique_lock<stdx::mutex> lk(_lastSyncMutex);
    uint32_t current = _lastSyncTime.loadRelaxed();  // synchronized with writes through mutex
    if (current != start) {
        // Someone else synced already since we read lastSyncTime, so we're done!
        return;
    }
    _lastSyncTime.store(current + 1);

    // Nobody has synched yet, so we have to sync ourselves.

    // This gets the token (OpTime) from the last write, before flushing (either the journal, or a
    // checkpoint), and then reports that token (OpTime) as a durable write.
    stdx::unique_lock<stdx::mutex> jlk(_journalListenerMutex);
    JournalListener::Token token = _journalListener->getToken();

    // Initialize on first use.
    if (!_waitUntilDurableSession) {
        invariantWTOK(
            _conn->open_session(_conn, NULL, "isolation=snapshot", &_waitUntilDurableSession));
    }

    // Use the journal when available, or a checkpoint otherwise.
    if (_engine && _engine->isDurable()) {
        invariantWTOK(_waitUntilDurableSession->log_flush(_waitUntilDurableSession, "sync=on"));
        LOG(4) << "flushed journal";
    } else {
        invariantWTOK(_waitUntilDurableSession->checkpoint(_waitUntilDurableSession, NULL));
        LOG(4) << "created checkpoint";
    }
    _journalListener->onDurable(token);
}

void WiredTigerSessionCache::waitUntilPreparedUnitOfWorkCommitsOrAborts(OperationContext* opCtx) {
    invariant(opCtx);
    stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);

    auto lastCounter = _lastCommitOrAbortCounter;
    opCtx->waitForConditionOrInterrupt(_prepareCommittedOrAbortedCond, lk, [&] {
        return lastCounter != _lastCommitOrAbortCounter;
    });
}

void WiredTigerSessionCache::notifyPreparedUnitOfWorkHasCommittedOrAborted() {
    {
        stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);
        _lastCommitOrAbortCounter++;
    }
    _prepareCommittedOrAbortedCond.notify_all();
}


void WiredTigerSessionCache::closeAllCursors(const std::string& uri) {
    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    for (SessionCache::iterator i = _sessions.begin(); i != _sessions.end(); i++) {
        (*i)->closeAllCursors(uri);
    }
}

void WiredTigerSessionCache::closeCursorsForQueuedDrops() {
    // Increment the cursor epoch so that all cursors from this epoch are closed.
    _cursorEpoch.fetchAndAdd(1);

    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    for (SessionCache::iterator i = _sessions.begin(); i != _sessions.end(); i++) {
        (*i)->closeCursorsForQueuedDrops(_engine);
    }
}

void WiredTigerSessionCache::closeAll() {
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

bool WiredTigerSessionCache::isEphemeral() {
    return _engine && _engine->isEphemeral();
}

UniqueWiredTigerSession WiredTigerSessionCache::getSession() {
    // We should never be able to get here after _shuttingDown is set, because no new
    // operations should be allowed to start.
    invariant(!(_shuttingDown.loadRelaxed() & kShuttingDownMask));

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        if (!_sessions.empty()) {
            // Get the most recently used session so that if we discard sessions, we're
            // discarding older ones
            WiredTigerSession* cachedSession = _sessions.back();
            _sessions.pop_back();
            return UniqueWiredTigerSession(cachedSession);
        }
    }

    // Outside of the cache partition lock, but on release will be put back on the cache
    return UniqueWiredTigerSession(
        new WiredTigerSession(_conn, this, _epoch.load(), _cursorEpoch.load()));
}

void WiredTigerSessionCache::releaseSession(WiredTigerSession* session) {
    invariant(session);
    invariant(session->cursorsOut() == 0);

    const int shuttingDown = _shuttingDown.fetchAndAdd(1);
    ON_BLOCK_EXIT([this] { _shuttingDown.fetchAndSubtract(1); });

    if (shuttingDown & kShuttingDownMask) {
        // There is a race condition with clean shutdown, where the storage engine is ripped from
        // underneath OperationContexts, which are not "active" (i.e., do not have any locks), but
        // are just about to delete the recovery unit. See SERVER-16031 for more information. Since
        // shutting down the WT_CONNECTION will close all WT_SESSIONS, we shouldn't also try to
        // directly close this session.
        session->_session = nullptr;  // Prevents calling _session->close() in destructor.
        delete session;
        return;
    }

    {
        WT_SESSION* ss = session->getSession();
        uint64_t range;
        // This checks that we are only caching idle sessions and not something which might hold
        // locks or otherwise prevent truncation.
        invariantWTOK(ss->transaction_pinned_range(ss, &range));
        invariant(range == 0);

        // Release resources in the session we're about to cache.
        // If we are using hybrid caching, then close cursors now and let them
        // be cached at the WiredTiger level.
        if (kWiredTigerCursorCacheSize.load() < 0) {
            session->closeAllCursors("");
        }
        invariantWTOK(ss->reset(ss));
    }

    // If the cursor epoch has moved on, close all cursors in the session.
    uint64_t cursorEpoch = _cursorEpoch.load();
    if (session->_getCursorEpoch() != cursorEpoch)
        session->closeCursorsForQueuedDrops(_engine);

    bool returnedToCache = false;
    uint64_t currentEpoch = _epoch.load();
    bool dropQueuedIdentsAtSessionEnd = session->isDropQueuedIdentsAtSessionEndAllowed();

    // Reset this session's flag for dropping queued idents to default, before returning it to
    // session cache.
    session->dropQueuedIdentsAtSessionEndAllowed(true);

    if (session->_getEpoch() == currentEpoch) {  // check outside of lock to reduce contention
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        if (session->_getEpoch() == _epoch.load()) {  // recheck inside the lock for correctness
            returnedToCache = true;
            _sessions.push_back(session);
        }
    } else
        invariant(session->_getEpoch() < currentEpoch);

    if (!returnedToCache)
        delete session;

    if (dropQueuedIdentsAtSessionEnd && _engine && _engine->haveDropsQueued())
        _engine->dropSomeQueuedIdents();
}


void WiredTigerSessionCache::setJournalListener(JournalListener* jl) {
    stdx::unique_lock<stdx::mutex> lk(_journalListenerMutex);
    _journalListener = jl;
}

bool WiredTigerSessionCache::isEngineCachingCursors() {
    return kWiredTigerCursorCacheSize.load() <= 0;
}

void WiredTigerSessionCache::WiredTigerSessionDeleter::operator()(
    WiredTigerSession* session) const {
    session->_cache->releaseSession(session);
}

}  // namespace mongo
