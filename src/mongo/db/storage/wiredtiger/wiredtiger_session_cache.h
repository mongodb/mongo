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
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/spin_lock.h"

namespace mongo {

class WiredTigerKVEngine;
class WiredTigerSessionCache;

class WiredTigerCachedCursor {
public:
    WiredTigerCachedCursor(uint64_t id, uint64_t gen, WT_CURSOR* cursor, const std::string& config)
        : _id(id), _gen(gen), _cursor(cursor), _config(config) {}

    uint64_t _id;   // Source ID, assigned to each URI
    uint64_t _gen;  // Generation, used to age out old cursors
    WT_CURSOR* _cursor;
    std::string _config;  // Cursor config. Do not serve cursors with different configurations
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
     * Gets a cursor on the table id 'id' with optional configuration, 'config'.
     *
     * This may return a cursor from the cursor cache and these cursors should *always* be released
     * into the cache by calling releaseCursor().
     */
    WT_CURSOR* getCachedCursor(uint64_t id, const std::string& config);


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
     * Wrapper for getNewCursor() without a config string.
     */
    WT_CURSOR* getNewCursor(const std::string& uri) {
        return getNewCursor(uri, nullptr);
    }

    /**
     * Release a cursor into the cursor cache and close old cursors if the number of cursors in the
     * cache exceeds wiredTigerCursorCacheSize.
     * The exact cursor config that was used to create the cursor must be provided or subsequent
     * users will retrieve cursors with incorrect configurations.
     */
    void releaseCursor(uint64_t id, WT_CURSOR* cursor, const std::string& config);

    /**
     * Close a cursor without releasing it into the cursor cache.
     */
    void closeCursor(WT_CURSOR* cursor);

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

    static uint64_t genTableId();

    /**
     * For special cursors. Guaranteed never to collide with genTableId() ids.
     */
    enum TableId {
        /* For "metadata:" cursors */
        kMetadataTableId,
        /* For "metadata:create" cursors */
        kMetadataCreateTableId,
        /* The start of non-special table ids for genTableId() */
        kLastTableId
    };

    void setIdleExpireTime(Date_t idleExpireTime) {
        _idleExpireTime = idleExpireTime;
    }

    Date_t getIdleExpireTime() const {
        return _idleExpireTime;
    }

private:
    friend class WiredTigerSessionCache;
    friend class WiredTigerKVEngine;

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
    int _cursorsOut;
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
     * Specifies what data will get flushed to disk in a WiredTigerSessionCache::waitUntilDurable()
     * call.
     */
    enum class Fsync {
        // Flushes only the journal (oplog) to disk.
        // If journaling is disabled, checkpoints all of the data.
        kJournal,
        // Checkpoints data up to the stable timestamp.
        // If journaling is disabled, checkpoints all of the data.
        kCheckpointStableTimestamp,
        // Checkpoints all of the data.
        kCheckpointAll,
    };

    /**
     * Controls whether or not WiredTigerSessionCache::waitUntilDurable() updates the
     * JournalListener.
     */
    enum class UseJournalListener { kUpdate, kSkip };

    // RAII type to block and unblock the WiredTigerSessionCache to shut down.
    class BlockShutdown {
    public:
        BlockShutdown(WiredTigerSessionCache* cache) : _cache(cache) {
            _cache->_shuttingDown.fetchAndAdd(1);
        }

        ~BlockShutdown() {
            _cache->_shuttingDown.fetchAndSubtract(1);
        }

    private:
        WiredTigerSessionCache* _cache;
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

    /**
     * True when in the process of shutting down.
     */
    bool isShuttingDown();

    bool isEphemeral();

    /**
     * Waits until all commits that happened before this call are made durable.
     *
     * Specifying Fsync::kJournal will flush only the (oplog) journal to disk. Callers are
     * serialized by a mutex and will return early if it is discovered that another thread started
     * and completed a flush while they slept.
     *
     * Specifying Fsync::kCheckpointStableTimestamp will take a checkpoint up to and including the
     * stable timestamp.
     *
     * Specifying Fsync::kCheckpointAll, or if journaling is disabled with kJournal or
     * kCheckpointStableTimestamp, causes a checkpoint to be taken of all of the data.
     *
     * Taking a checkpoint has the benefit of persisting unjournaled writes.
     *
     * 'useListener' controls whether or not the JournalListener is updated with the last durable
     * value of the timestamp that it tracks. The JournalListener's token is fetched before writing
     * out to disk and set afterwards to update the repl layer durable timestamp. The
     * JournalListener operations can throw write interruption errors.
     *
     * Uses a temporary session. Safe to call without any locks, even during shutdown.
     */
    void waitUntilDurable(OperationContext* opCtx, Fsync syncType, UseJournalListener useListener);

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
    //   The low 31 bits are a count of active calls that need to block shutdown.
    //   The high bit is a flag that is set if and only if we're shutting down.
    AtomicWord<unsigned> _shuttingDown;
    static const uint32_t kShuttingDownMask = 1 << 31;

    Mutex _cacheLock = MONGO_MAKE_LATCH("WiredTigerSessionCache::_cacheLock");
    typedef std::vector<WiredTigerSession*> SessionCache;
    SessionCache _sessions;

    // Bumped when all open sessions need to be closed
    AtomicWord<unsigned long long> _epoch;  // atomic so we can check it outside of the lock

    // Counter and critical section mutex for waitUntilDurable
    AtomicWord<unsigned> _lastSyncTime;
    Mutex _lastSyncMutex = MONGO_MAKE_LATCH("WiredTigerSessionCache::_lastSyncMutex");

    // Mutex and cond var for waiting on prepare commit or abort.
    Mutex _prepareCommittedOrAbortedMutex =
        MONGO_MAKE_LATCH("WiredTigerSessionCache::_prepareCommittedOrAbortedMutex");
    stdx::condition_variable _prepareCommittedOrAbortedCond;
    AtomicWord<std::uint64_t> _prepareCommitOrAbortCounter{0};

    // Protects getting and setting the _journalListener below.
    Mutex _journalListenerMutex = MONGO_MAKE_LATCH("WiredTigerSessionCache::_journalListenerMutex");

    // Notified when we commit to the journal.
    //
    // This variable should be accessed under the _journalListenerMutex above and saved in a local
    // variable before use. That way, we can avoid holding a mutex across calls on the object. It is
    // only allowed to be set once, in order to ensure the memory to which a copy of the pointer
    // points is always valid.
    JournalListener* _journalListener = nullptr;

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

static constexpr char kWTRepairMsg[] =
    "Please read the documentation for starting MongoDB with --repair here: "
    "http://dochub.mongodb.org/core/repair";
}  // namespace mongo
