// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_managed_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wiredtiger.h>

namespace mongo {

class StatsCollectionPermit;
class WiredTigerKVEngineBase;

/**
 *  This is a wrapper class for WT_CONNECTION and contains a shared pool of cached WiredTiger
 * sessions with the goal to amortize the cost of session creation and destruction over multiple
 * uses.
 */
class WiredTigerConnection {
public:
    WiredTigerConnection(WiredTigerKVEngineBase* engine, int32_t sessionCacheMax);
    WiredTigerConnection(WT_CONNECTION* conn,
                         ClockSource* cs,
                         int32_t sessionCacheMax,
                         WiredTigerKVEngineBase* engine = nullptr);
    ~WiredTigerConnection();


    // RAII type to block and unblock the WiredTigerConnection to shut down.
    class BlockShutdown {
    public:
        BlockShutdown(WiredTigerConnection* connection) : _connection(connection) {
            _connection->_shuttingDown.fetchAndAdd(1);
        }

        ~BlockShutdown() {
            _connection->_shuttingDown.fetchAndSubtract(1);
        }

        bool isShuttingDown() const {
            return _connection->isShuttingDown();
        }

    private:
        WiredTigerConnection* _connection{nullptr};
    };

    enum class ShutdownReason {
        kRollbackToStable,
        kCleanShutdown,
    };

    /**
     * Returns an RAII wrapper to a previously released session for reuse, or creates a new session.
     * This method must only be called while holding the global lock to avoid races with
     * shuttingDown, but otherwise is thread safe.
     * The passed in OperationContext is used to propagate interrupts from MongoDB to WiredTiger. If
     * interrupts are not needed call getUninterruptibleSession()
     */
    WiredTigerManagedSession getSession(OperationContext& interruptible,
                                        const char* config = nullptr);

    /**
     * As above but does not propagate interrupts
     */
    WiredTigerManagedSession getUninterruptibleSession(const char* config = nullptr,
                                                       bool isInternal = true);

    /**
     * Get the maximum number of sessions allowed in the cache.
     */
    int32_t getSessionCacheMax() const;

    /**
     * Get a count of idle sessions in the session cache.
     */
    size_t getIdleSessionsCount();

    /**
     * Get the amount of time spent "inside" the engine, i.e. calling wiredtiger APIs.
     *
     * Note: This only counts "completed" sessions, time spent by active sessions is displayed in
     * the curop until those sessions are returned to the connection's cache.
     */
    Microseconds getTotalEngineTime();

    /**
     * Closes all cached sessions whose idle expiration time has been reached.
     */
    void closeExpiredIdleSessions(int64_t idleTimeMillis);

    /**
     * Free all cached sessions and ensures that previously acquired sessions will be freed on
     * release.
     */
    void closeAll(ShutdownReason reason);

    /**
     * Transitions the cache to shutting down mode. Any already released sessions are freed and
     * any sessions released subsequently are leaked. Must be called while holding the global
     * lock in exclusive mode to avoid races with getSession.
     * @param reason Reason for shutting down.
     */
    void shuttingDown(ShutdownReason reason);

    /**
     * True when in the process of shutting down.
     */
    bool isShuttingDown();

    /**
     * True only when shutting down for a reason that tears down the underlying WT_CONNECTION
     * (kCleanShutdown). False when not shutting down or when transiently shutting down for a
     * rollback to stable, in which case the WT_CONNECTION remains valid.
     */
    bool isCleanShuttingDown();

    /**
     * Restart a previously shut down cache.
     */
    void restart();

    bool isEphemeral();

    /**
     * Waits until a prepared unit of work has ended (either been commited or aborted). This
     * should be used when encountering WT_PREPARE_CONFLICT errors. The caller is required to retry
     * the conflicting WiredTiger API operation. A return from this function does not guarantee that
     * the conflicting transaction has ended, only that one prepared unit of work in the process has
     * signaled that it has ended.
     * Accepts an Interruptible that will throw an AssertionException when interrupted.
     *
     * This method is provided in WiredTigerConnection and not RecoveryUnit because all recovery
     * units share the same session cache, and we want a recovery unit on one thread to signal all
     * recovery units waiting for prepare conflicts across all other threads.
     */
    void waitUntilPreparedUnitOfWorkCommitsOrAborts(Interruptible& interruptible,
                                                    uint64_t lastCount);

    /**
     * Notifies waiters that the caller's prepared unit of work has ended (either committed or
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

    WiredTigerKVEngineBase* getKVEngine() const {
        return _engine;
    }

    std::uint64_t getPrepareCommitOrAbortCount() const {
        return _prepareCommitOrAbortCounter.loadRelaxed();
    }

    CompiledConfigurationsPerConnection* getCompiledConfigurations() {
        return &_compiledConfigurations;
    }

private:
    // Opens a session.
    WT_SESSION* _openSession(WiredTigerSession* session,
                             WT_EVENT_HANDLER* handler,
                             const char* config);

    // Similar to _openSession(), but uses the connection from the provided permit. This is
    // necessary when using sessions that are concurrent with shutdown, such as for statistics.
    WT_SESSION* _openSession(WiredTigerSession* session,
                             StatsCollectionPermit& permit,
                             const char* config);

    // Similar to the above but accepts an event handler.
    WT_SESSION* _openSession(WiredTigerSession* session,
                             WT_EVENT_HANDLER* handler,
                             StatsCollectionPermit& permit,
                             const char* config);

    // Similar to the above, but opens a session using the provided connection.
    WT_SESSION* _openSessionInternal(WiredTigerSession* session,
                                     WT_EVENT_HANDLER* handler,
                                     const char* config,
                                     WT_CONNECTION* conn);

    /**
     * Returns a session to the cache for later reuse. If closeAll was called between getting this
     * session and releasing it, the session is directly released. This method is thread safe.
     */
    void _releaseSession(std::unique_ptr<WiredTigerSession> session);

    /*
     * Closes the given session by decrementing the appropriate session counters.
     */
    void _closeSession(bool isInternalSession);

    /**
     * Increments the total session count and, for internal sessions, also increments
     * the reserved internal session count.
     */
    void _incrementSessionCount(bool isInternalSession);

    /**
     * Decrements the total session count and, for internal sessions, also decrements
     * the reserved internal session count.
     */
    void _decrementSessionCount(bool isInternalSession);

    friend class WiredTigerSession;
    friend class WiredTigerManagedSession;
    WT_CONNECTION* _conn;             // not owned
    ClockSource* const _clockSource;  // not owned
    const int32_t _sessionCacheMax;
    WiredTigerKVEngineBase* _engine{nullptr};  // not owned, might be NULL
    WiredTigerSnapshotManager _snapshotManager;
    CompiledConfigurationsPerConnection _compiledConfigurations;

    // Used as follows:
    //   The low 30 bits are a count of active calls that need to block shutdown.
    //   Bit 30 is set iff the in-progress shutdown is a kCleanShutdown (the WT_CONNECTION will be
    //     torn down). Set together with kShuttingDownMask; cleared by restart().
    //   Bit 31 is set if and only if we're shutting down.
    Atomic<unsigned> _shuttingDown{0};
    static const uint32_t kShuttingDownMask = 1u << 31;
    static const uint32_t kCleanShutdownMask = 1u << 30;

    std::mutex _cacheLock;
    typedef std::vector<std::unique_ptr<WiredTigerSession>> SessionCache;
    SessionCache _sessions;
    Microseconds _totalEngineTime;  // protected by _cacheLock

    // We track two epochs here:
    //  Engine epoch: bumped when the storage engine reloads. If out of sync, we return without
    //      caching the cursor and skip closing it, since session teardown will handle closure.
    //  RTS (rollback-to-stable) epoch: bumped on rollback-to-stable. If out of sync, we must close
    //      the cursor immediately without caching to prevent leaking the cursor since the cursor is
    //      no longer tracked.
    //  The engine epoch takes precedence over the RTS epoch and should be checked first.
    Atomic<unsigned long long> _engineEpoch;  // atomic so we can check it outside of the lock
    Atomic<unsigned long long> _rtsEpoch;     // atomic so we can check it outside of the lock

    // Mutex and cond var for waiting on prepare commit or abort.
    std::mutex _prepareCommittedOrAbortedMutex;
    stdx::condition_variable _prepareCommittedOrAbortedCond;
    Atomic<std::uint64_t> _prepareCommitOrAbortCounter{0};

    Atomic<int> _openSessions{0};
    Atomic<int> _openUserSessions{0};
};

static constexpr char kWTRepairMsg[] =
    "Please read the documentation for starting MongoDB with --repair here: "
    "http://dochub.mongodb.org/core/repair";
}  // namespace mongo
