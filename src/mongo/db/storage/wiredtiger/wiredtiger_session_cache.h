// wiredtiger_session_cache.h

/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <list>
#include <string>

#include <boost/thread/shared_mutex.hpp>
#include <wiredtiger.h>

#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/spin_lock.h"

namespace mongo {

class WiredTigerKVEngine;
class WiredTigerSessionCache;

class WiredTigerCachedCursor {
public:
    WiredTigerCachedCursor(uint64_t id, uint64_t gen, WT_CURSOR* cursor)
        : _id(id), _gen(gen), _cursor(cursor) {}

    uint64_t _id;   // Source ID, assigned to each URI
    uint64_t _gen;  // Generation, used to age out old cursors
    WT_CURSOR* _cursor;
};

/**
 * This is a structure that caches 1 cursor for each uri.
 * The idea is that there is a pool of these somewhere.
 * NOT THREADSAFE
 */
class WiredTigerSession {
public:
    /**
     * Creates a new WT session on the specified connection.
     *
     * @param conn WT connection
     * @param epoch In which session cache cleanup epoch was this session instantiated. Value
     *          of -1 means that this value is not necessary since the session will not be
     *          cached.
     */
    WiredTigerSession(WT_CONNECTION* conn, int epoch = -1);

    /**
     * Creates a new WT session on the specified connection.
     *
     * @param conn WT connection
     * @param cache The WiredTigerSessionCache that owns this session.
     * @param epoch In which session cache cleanup epoch was this session instantiated. Value
     *          of -1 means that this value is not necessary since the session will not be
     *          cached.
     */
    WiredTigerSession(WT_CONNECTION* conn, WiredTigerSessionCache* cache, int epoch = -1);

    ~WiredTigerSession();

    WT_SESSION* getSession() const {
        return _session;
    }

    WT_CURSOR* getCursor(const std::string& uri, uint64_t id, bool forRecordStore);

    void releaseCursor(uint64_t id, WT_CURSOR* cursor);

    void closeAllCursors();

    int cursorsOut() const {
        return _cursorsOut;
    }

    static uint64_t genTableId();

    /**
     * For "metadata:" cursors. Guaranteed never to collide with genTableId() ids.
     */
    static const uint64_t kMetadataTableId = 0;

private:
    friend class WiredTigerSessionCache;

    // The cursor cache is a list of pairs that contain an ID and cursor
    typedef std::list<WiredTigerCachedCursor> CursorCache;

    // Used internally by WiredTigerSessionCache
    uint64_t _getEpoch() const {
        return _epoch;
    }

    const uint64_t _epoch;
    WiredTigerSessionCache* _cache;  // not owned
    WT_SESSION* _session;            // owned
    CursorCache _cursors;            // owned
    uint64_t _cursorGen;
    int _cursorsCached, _cursorsOut;
};

/**
 *  This cache implements a shared pool of WiredTiger sessions with the goal to amortize the
 *  cost of session creation and destruction over multiple uses.
 */
class WiredTigerSessionCache {
public:
    WiredTigerSessionCache(WiredTigerKVEngine* engine);
    WiredTigerSessionCache(WT_CONNECTION* conn);
    ~WiredTigerSessionCache();

    /**
     * This deleter automatically releases WiredTigerSession objects when no longer needed.
     */
    class WiredTigerSessionDeleter {
    public:
        void operator()(WiredTigerSession* session) const;
    };

    /**
     * Returns a smart pointer to a previously released session for reuse, or creates a new session.
     * This method must only be called while holding the global lock to avoid races with
     * shuttingDown, but otherwise is thread safe.
     */
    std::unique_ptr<WiredTigerSession, WiredTigerSessionDeleter> getSession();

    /**
     * Free all cached sessions and ensures that previously acquired sessions will be freed on
     * release.
     */
    void closeAll();

    /**
     * Transitions the cache to shutting down mode. Any already released sessions are freed and
     * any sessions released subsequently are leaked. Must be called while holding the global
     * lock in exclusive mode to avoid races with getSession.
     */
    void shuttingDown();

    bool isEphemeral();
    /**
     * Waits until all commits that happened before this call are durable, either by flushing
     * the log or forcing a checkpoint if forceCheckpoint is true or the journal is disabled.
     * Uses a temporary session. Safe to call without any locks, even during shutdown.
     */
    void waitUntilDurable(bool forceCheckpoint);

    WT_CONNECTION* conn() const {
        return _conn;
    }

    WiredTigerSnapshotManager& snapshotManager() {
        return _snapshotManager;
    }
    const WiredTigerSnapshotManager& snapshotManager() const {
        return _snapshotManager;
    }

    void setJournalListener(JournalListener* jl);

private:
    WiredTigerKVEngine* _engine;  // not owned, might be NULL
    WT_CONNECTION* _conn;         // not owned
    WiredTigerSnapshotManager _snapshotManager;

    // Used as follows:
    //   The low 31 bits are a count of active calls to releaseSession.
    //   The high bit is a flag that is set if and only if we're shutting down.
    AtomicUInt32 _shuttingDown;
    static const uint32_t kShuttingDownMask = 1 << 31;

    stdx::mutex _cacheLock;
    typedef std::vector<WiredTigerSession*> SessionCache;
    SessionCache _sessions;

    // Bumped when all open sessions need to be closed
    AtomicUInt64 _epoch;  // atomic so we can check it outside of the lock

    // Counter and critical section mutex for waitUntilDurable
    AtomicUInt32 _lastSyncTime;
    stdx::mutex _lastSyncMutex;

    // Notified when we commit to the journal.
    JournalListener* _journalListener = &NoOpJournalListener::instance;
    // Protects _journalListener.
    stdx::mutex _journalListenerMutex;

    /**
     * Returns a session to the cache for later reuse. If closeAll was called between getting this
     * session and releasing it, the session is directly released. This method is thread safe.
     */
    void releaseSession(WiredTigerSession* session);
};

/**
 * A unique handle type for WiredTigerSession pointers obtained from a WiredTigerSessionCache.
 */
typedef std::unique_ptr<WiredTigerSession,
                        typename WiredTigerSessionCache::WiredTigerSessionDeleter>
    UniqueWiredTigerSession;
}  // namespace
