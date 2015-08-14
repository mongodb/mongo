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

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn, int epoch)
    : _epoch(epoch), _session(NULL), _cursorGen(0), _cursorsCached(0), _cursorsOut(0) {
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
            _cursorsCached--;
            return c;
        }
    }

    WT_CURSOR* c = NULL;
    int ret = _session->open_cursor(
        _session, uri.c_str(), NULL, forRecordStore ? "" : "overwrite=false", &c);
    if (ret != ENOENT)
        invariantWTOK(ret);
    if (c)
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
    _cursorsCached++;

    // "Old" is defined as not used in the last N**2 operations, if we have N cursors cached.
    // The reasoning here is to imagine a workload with N tables performing operations randomly
    // across all of them (i.e., each cursor has 1/N chance of used for each operation).  We
    // would like to cache N cursors in that case, so any given cursor could go N**2 operations
    // in between use.
    while (_cursorGen - _cursors.back()._gen > 10000) {
        cursor = _cursors.back()._cursor;
        _cursors.pop_back();
        _cursorsCached--;
        invariantWTOK(cursor->close(cursor));
    }
}

void WiredTigerSession::closeAllCursors() {
    invariant(_session);
    for (CursorCache::iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        WT_CURSOR* cursor = i->_cursor;
        if (cursor) {
            invariantWTOK(cursor->close(cursor));
        }
    }
    _cursors.clear();
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
    : _engine(engine), _conn(engine->getConnection()), _snapshotManager(_conn), _shuttingDown(0) {}

WiredTigerSessionCache::WiredTigerSessionCache(WT_CONNECTION* conn)
    : _engine(NULL), _conn(conn), _snapshotManager(_conn), _shuttingDown(0) {}

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
    _snapshotManager.shutdown();
}

void WiredTigerSessionCache::closeAll() {
    // Increment the epoch as we are now closing all sessions with this epoch
    SessionCache swap;

    {
        stdx::lock_guard<SpinLock> lock(_cacheLock);
        _epoch.fetchAndAdd(1);
        _sessions.swap(swap);
    }

    for (SessionCache::iterator i = swap.begin(); i != swap.end(); i++) {
        delete (*i);
    }
}

WiredTigerSession* WiredTigerSessionCache::getSession() {
    // We should never be able to get here after _shuttingDown is set, because no new
    // operations should be allowed to start.
    invariant(!(_shuttingDown.loadRelaxed() & kShuttingDownMask));

    {
        stdx::lock_guard<SpinLock> lock(_cacheLock);
        if (!_sessions.empty()) {
            // Get the most recently used session so that if we discard sessions, we're
            // discarding older ones
            WiredTigerSession* cachedSession = _sessions.back();
            _sessions.pop_back();
            return cachedSession;
        }
    }

    // Outside of the cache partition lock, but on release will be put back on the cache
    return new WiredTigerSession(_conn, _epoch.load());
}

void WiredTigerSessionCache::releaseSession(WiredTigerSession* session) {
    invariant(session);
    invariant(session->cursorsOut() == 0);

    const int shuttingDown = _shuttingDown.fetchAndAdd(1);
    ON_BLOCK_EXIT([this] { _shuttingDown.fetchAndSubtract(1); });

    if (shuttingDown & kShuttingDownMask) {
        // Leak the session in order to avoid race condition with clean shutdown, where the
        // storage engine is ripped from underneath transactions, which are not "active"
        // (i.e., do not have any locks), but are just about to delete the recovery unit.
        // See SERVER-16031 for more information.
        return;
    }

    // This checks that we are only caching idle sessions and not something which might hold
    // locks or otherwise prevent truncation.
    {
        WT_SESSION* ss = session->getSession();
        uint64_t range;
        invariantWTOK(ss->transaction_pinned_range(ss, &range));
        invariant(range == 0);
    }

    bool returnedToCache = false;
    uint64_t currentEpoch = _epoch.load();

    if (session->_getEpoch() == currentEpoch) {  // check outside of lock to reduce contention
        stdx::lock_guard<SpinLock> lock(_cacheLock);
        if (session->_getEpoch() == _epoch.load()) {  // recheck inside the lock for correctness
            returnedToCache = true;
            _sessions.push_back(session);
        }
    } else
        invariant(session->_getEpoch() < currentEpoch);

    if (!returnedToCache)
        delete session;

    if (_engine && _engine->haveDropsQueued())
        _engine->dropAllQueued();
}
}
