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

#include <list>
#include <string>

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
     * @param epoch In which session cache cleanup epoch was this session instantiated.
     * @param cursorEpoch In which cursor cache cleanup epoch was this session instantiated.
     */
    WiredTigerSession(WT_CONNECTION* conn, uint64_t epoch = 0, uint64_t cursorEpoch = 0);

    /**
     * Creates a new WT session on the specified connection.
     *
     * @param conn WT connection
     * @param cache The WiredTigerSessionCache that owns this session.
     * @param epoch In which session cache cleanup epoch was this session instantiated.
     * @param cursorEpoch In which cursor cache cleanup epoch was this session instantiated.
     */
    WiredTigerSession(WT_CONNECTION* conn,
                      WiredTigerSessionCache* cache,
                      uint64_t epoch = 0,
                      uint64_t cursorEpoch = 0);

    ~WiredTigerSession();

    WT_SESSION* getSession() const {
        return _session;
    }

    /**
     * Gets a cursor on the table id 'id'.
     *
     * The config string specifies optional arguments for the cursor. For example, when
     * the config contains 'read_once=true', this is intended for operations that will be
     * sequentially scanning large amounts of data. If no cursor is currently available in the
     * cursor cache, then a new cached cursor will be created with the given config specification.
     *
     * This may return a cursor from the cursor cache and these cursors should *always* be released
     * into the cache by calling releaseCursor().
     */
    WT_CURSOR* getCachedCursor(const std::string& uri, uint64_t id, const char* config);


    /**
     * Create a new cursor and ignore the cache.
     *
     * The config string specifies optional arguments for the cursor. For example, when
     * the config contains 'read_once=true', this is intended for operations that will be
     * sequentially scanning large amounts of data.
     *
     * This will never return a cursor from the cursor cache, and these cursors should *never* be
     * released into the cache by calling releaseCursor(). Use closeCursor() instead.
     */
    WT_CURSOR* getNewCursor(const std::string& uri, const char* config);

    /**
     * Release a cursor into the cursor cache and close old cursors if the number of cursors in the
     * cache exceeds wiredTigerCursorCacheSize.
     */
    void releaseCursor(uint64_t id, WT_CURSOR* cursor);

    /**
     * Close a cursor without releasing it into the cursor cache.
     */
    void closeCursor(WT_CURSOR* cursor);

    void closeCursorsForQueuedDrops(WiredTigerKVEngine* engine);

    /**
     * Closes all cached cursors matching the uri.  If the uri is empty,
     * all cached cursors are closed.
     */
    void closeAllCursors(const std::string& uri);

    int cursorsOut() const {
        return _cursorsOut;
    }

    int cachedCursors() const {
        return _cursors.size();
    }

    bool isDropQueuedIdentsAtSessionEndAllowed() const {
        return _dropQueuedIdentsAtSessionEnd;
    }

    void dropQueuedIdentsAtSessionEndAllowed(bool dropQueuedIdentsAtSessionEnd) {
        _dropQueuedIdentsAtSessionEnd = dropQueuedIdentsAtSessionEnd;
    }

    static uint64_t genTableId();

    /**
     * For "metadata:" cursors. Guaranteed never to collide with genTableId() ids.
     */
    static const uint64_t kMetadataTableId = 0;

    void setIdleExpireTime(Date_t idleExpireTime) {
        _idleExpireTime = idleExpireTime;
    }

    Date_t getIdleExpireTime() const {
        return _idleExpireTime;
    }

private:
    friend class WiredTigerSessionCache;

    // The cursor cache is a list of pairs that contain an ID and cursor
    typedef std::list<WiredTigerCachedCursor> CursorCache;

    // Used internally by WiredTigerSessionCache
    uint64_t _getEpoch() const {
        return _epoch;
    }

    // Used internally by WiredTigerSessionCache
    uint64_t _getCursorEpoch() const {
        return _cursorEpoch;
    }

    const uint64_t _epoch;
    uint64_t _cursorEpoch;
    WiredTigerSessionCache* _cache;  // not owned
    WT_SESSION* _session;            // owned
    CursorCache _cursors;            // owned
    uint64_t _cursorGen;
    int _cursorsOut;
    bool _dropQueuedIdentsAtSessionEnd = true;
    Date_t _idleExpireTime;
};

/**
 *  This cache implements a shared pool of WiredTiger sessions with the goal to amortize the
 *  cost of session creation and destruction over multiple uses.
 */
class WiredTigerSessionCache {
public:
    WiredTigerSessionCache(WiredTigerKVEngine* engine);
    WiredTigerSessionCache(WT_CONNECTION* conn, ClockSource* cs);
    ~WiredTigerSessionCache();

    /**
     * This deleter automatically releases WiredTigerSession objects when no longer needed.
     */
    class WiredTigerSessionDeleter {
    public:
        void operator()(WiredTigerSession* session) const;
    };

    /**
     * Indicates that WiredTiger should be configured to cache cursors.
     */
    static bool isEngineCachingCursors();

    /**
     * Returns a smart pointer to a previously released session for reuse, or creates a new session.
     * This method must only be called while holding the global lock to avoid races with
     * shuttingDown, but otherwise is thread safe.
     */
    std::unique_ptr<WiredTigerSession, WiredTigerSessionDeleter> getSession();

    /**
     * Get a count of idle sessions in the session cache.
     */
    size_t getIdleSessionsCount();

    /**
     * Closes all cached sessions whose idle expiration time has been reached.
     */
    void closeExpiredIdleSessions(int64_t idleTimeMillis);

    /**
     * Free all cached sessions and ensures that previously acquired sessions will be freed on
     * release.
     */
    void closeAll();

    /**
     * Closes cached cursors for tables that are queued to be dropped.
     */
    void closeCursorsForQueuedDrops();

    /**
     * Closes all cached cursors matching the uri.  If the uri is empty,
     * all cached cursors are closed.
     */
    void closeAllCursors(const std::string& uri);

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
    void waitUntilDurable(bool forceCheckpoint, bool stableCheckpoint);

    /**
     * Waits until a prepared unit of work has ended (either been commited or aborted). This
     * should be used when encountering WT_PREPARE_CONFLICT errors. The caller is required to retry
     * the conflicting WiredTiger API operation. A return from this function does not guarantee that
     * the conflicting transaction has ended, only that one prepared unit of work in the process has
     * signaled that it has ended.
     * Accepts an OperationContext that will throw an AssertionException when interrupted.
     *
     * This method is provided in WiredTigerSessionCache and not RecoveryUnit because all recovery
     * units share the same session cache, and we want a recovery unit on one thread to signal all
     * recovery units waiting for prepare conflicts across all other threads.
     */
    void waitUntilPreparedUnitOfWorkCommitsOrAborts(OperationContext* opCtx, uint64_t lastCount);

    /**
     * Notifies waiters that the caller's perpared unit of work has ended (either committed or
     * aborted).
     */
    void notifyPreparedUnitOfWorkHasCommittedOrAborted();

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

    uint64_t getCursorEpoch() const {
        return _cursorEpoch.load();
    }

    WiredTigerKVEngine* getKVEngine() const {
        return _engine;
    }

    std::uint64_t getPrepareCommitOrAbortCount() const {
        return _prepareCommitOrAbortCounter.loadRelaxed();
    }

private:
    WiredTigerKVEngine* _engine;      // not owned, might be NULL
    WT_CONNECTION* _conn;             // not owned
    ClockSource* const _clockSource;  // not owned
    WiredTigerSnapshotManager _snapshotManager;

    // Used as follows:
    //   The low 31 bits are a count of active calls to releaseSession.
    //   The high bit is a flag that is set if and only if we're shutting down.
    AtomicWord<unsigned> _shuttingDown;
    static const uint32_t kShuttingDownMask = 1 << 31;

    stdx::mutex _cacheLock;
    typedef std::vector<WiredTigerSession*> SessionCache;
    SessionCache _sessions;

    // Bumped when all open sessions need to be closed
    AtomicWord<unsigned long long> _epoch;  // atomic so we can check it outside of the lock

    // Bumped when all open cursors need to be closed
    AtomicWord<unsigned long long> _cursorEpoch;  // atomic so we can check it outside of the lock

    // Counter and critical section mutex for waitUntilDurable
    AtomicWord<unsigned> _lastSyncTime;
    stdx::mutex _lastSyncMutex;

    // Mutex and cond var for waiting on prepare commit or abort.
    stdx::mutex _prepareCommittedOrAbortedMutex;
    stdx::condition_variable _prepareCommittedOrAbortedCond;
    AtomicWord<std::uint64_t> _prepareCommitOrAbortCounter{0};

    // Protects _journalListener.
    stdx::mutex _journalListenerMutex;
    // Notified when we commit to the journal.
    JournalListener* _journalListener = &NoOpJournalListener::instance;

    WT_SESSION* _waitUntilDurableSession = nullptr;  // owned, and never explicitly closed
                                                     // (uses connection close to clean up)

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

extern const std::string kWTRepairMsg;
}  // namespace mongo
