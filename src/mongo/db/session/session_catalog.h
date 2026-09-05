// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ObservableSession;

namespace session_catalog_detail {
/**
 * The partition which the calling thread currently holds through a 'Locked', or null. Thread-
 * private, so no atomic is needed: the only thread whose read must be correct is the one which
 * wrote it. Partitions are never locked two-at-a-time (every scan scopes its 'Locked' inside the
 * per-partition loop), so a single slot suffices; the saved previous value in 'LockedImpl' keeps
 * the answer honest if that ever changes.
 */
inline const void*& heldPartition() {
    thread_local const void* held = nullptr;
    return held;
}
}  // namespace session_catalog_detail

/**
 * Keeps track of the transaction runtime state for every active transaction session on this
 * instance.
 */
class [[MONGO_MOD_PUBLIC]] SessionCatalog {
    SessionCatalog(const SessionCatalog&) = delete;
    SessionCatalog& operator=(const SessionCatalog&) = delete;

    friend class ObservableSession;
    friend class OperationContextSession;

public:
    /**
     * Represents which role the SessionCatalog was accessed in. The participant role for actions
     * from a data bearing node (e.g. mongod servicing a local command) and router for a routing
     * node (e.g. a mongos command, or mongod running a mongos command).
     */
    enum class Provenance { kParticipant, kRouter };

    using TxnNumberAndProvenance = std::pair<TxnNumber, Provenance>;

    using ScanSessionsCallbackFn = std::function<void(ObservableSession&)>;
    using ScanSessionsReadOnlyCallbackFn = std::function<void(const ObservableSession&)>;
    using KillSessionsPredicateFn = std::function<bool(const ObservableSession&)>;
    using OnEagerlyReapedSessionsFn =
        unique_function<void(ServiceContext*, std::vector<LogicalSessionId>)>;
    using MakeSessionWorkerFnForEagerReap =
        unique_function<ScanSessionsCallbackFn(ServiceContext*, TxnNumber, Provenance)>;

    class ScopedCheckedOutSession;
    class SessionToKill;

    /**
     * RAII handle for an outstanding kill request on 'lsidToKill'. However the token goes away -
     * consumed, moved from, or destroyed - the kill is returned to the catalog exactly once.
     *
     * NOTE: returning a kill locks the session's partition, so a token must not be destroyed while
     * that partition is held. Tokens are only ever handed out with it released.
     */
    class KillToken {
    public:
        KillToken(KillToken&& other) noexcept
            : _lsidToKill(std::move(other._lsidToKill)),
              _catalog(std::exchange(other._catalog, nullptr)) {}

        KillToken& operator=(KillToken&& other) noexcept {
            if (this != &other) {
                _returnIfArmed();
                _lsidToKill = std::move(other._lsidToKill);
                _catalog = std::exchange(other._catalog, nullptr);
            }
            return *this;
        }

        KillToken(const KillToken&) = delete;
        KillToken& operator=(const KillToken&) = delete;

        ~KillToken() {
            _returnIfArmed();
        }

        const LogicalSessionId& lsidToKill() const {
            return _lsidToKill;
        }

    private:
        // Only 'ObservableSession::kill' may issue a token: one fabricated elsewhere would
        // return a kill which was never requested.
        friend class ObservableSession;
        friend class SessionCatalog;

        KillToken(SessionCatalog* catalog, LogicalSessionId lsid)
            : _lsidToKill(std::move(lsid)), _catalog(catalog) {}

        void _returnIfArmed();

        // The session whose kill this returns. Only ever set together with '_catalog', so the two
        // cannot get out of step.
        LogicalSessionId _lsidToKill;

        // Null once the kill has been returned or the token has been moved from.
        SessionCatalog* _catalog;
    };

    SessionCatalog();
    ~SessionCatalog();

    /**
     * Retrieves the session transaction table associated with the service or operation context.
     */
    static SessionCatalog* get(OperationContext* opCtx);
    static SessionCatalog* get(ServiceContext* service);

    /**
     * Resets the transaction table to an uninitialized state.
     * Meant only for testing.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void reset_forTest();

    /**
     * See the description of 'ObservableSession::kill' for more information on the session kill
     * usage pattern.
     */
    SessionToKill checkOutSessionForKill(OperationContext* opCtx,
                                         KillToken killToken,
                                         Date_t deadline = Date_t::max());

    /**
     * Iterates through the SessionCatalog under the SessionCatalog mutex and applies 'workerFn'
     * to the Session matching the specified 'lsid'. Does not support reaping.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by 'workerFn' is
     * not allowed to block, perform I/O or acquire any lock manager locks.
     */
    enum class ScanSessionCreateSession { kYes, kNo };
    void scanSession(const LogicalSessionId& lsid,
                     const ScanSessionsCallbackFn& perSessionScanFn,
                     ScanSessionCreateSession createSession = ScanSessionCreateSession::kNo);

    /**
     * Iterates through all sessions matching 'matcher' and kills those for which 'shouldKill'
     * returns true (or all matched sessions if 'shouldKill' is nullptr). If 'perSessionScanFn' is
     * provided, it is called for each matched session for read-only data collection before the kill
     * decision. Returns the collected     KillTokens.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by 'shouldKill'
     * and 'perSessionScanFn' is not allowed to block, perform I/O or acquire any lock manager
     * locks.
     */
    std::vector<KillToken> killSessions(
        const SessionKiller::Matcher& matcher,
        ErrorCodes::Error reason = ErrorCodes::Interrupted,
        const KillSessionsPredicateFn& shouldKill = nullptr,
        const ScanSessionsReadOnlyCallbackFn& perSessionScanFn = nullptr);

    /**
     * Returns the set of parent session IDs whose last checkout time is before 'threshold'.
     */
    LogicalSessionIdSet findExpiredParentSessions(Date_t threshold) const;

    /**
     * Iterates through all sessions matching 'matcher' and applies 'perSessionScanFn' to each
     * session for read-only access.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by
     * 'perSessionScanFn' is not allowed to block, perform I/O or acquire any lock manager locks.
     */
    void scanSessions(const SessionKiller::Matcher& matcher,
                      const ScanSessionsReadOnlyCallbackFn& perSessionScanFn);

    /**
     * Same as the above but applies 'parentSessionWorkerFn' to the Session whose session id is
     * equal to 'parentLsid' and then applies 'childSessionWorkerFn' to the Sessions whose parent
     * session id is equal to 'parentLsid'. To be used with 'markForReap' for reaping sessions
     * from the SessionCatalog. It enables transaction sessions that corresponds to the same
     * logical session to be reaped atomically. Returns the session ids for the matching Sessions
     * that were not reaped after the scan.
     */
    LogicalSessionIdSet scanSessionsForReap(const LogicalSessionId& parentLsid,
                                            const ScanSessionsCallbackFn& parentSessionWorkerFn,
                                            const ScanSessionsCallbackFn& childSessionWorkerFn);

    /**
     * Shortcut to invoke 'kill' on the specified session under the SessionCatalog mutex. Throws a
     * NoSuchSession exception if the session doesn't exist.
     */
    KillToken killSession(const LogicalSessionId& lsid,
                          ErrorCodes::Error reason = ErrorCodes::Interrupted);

    /**
     * Kills the session with 'lsid' if 'shouldKill' returns true for it, and returns the resulting
     * token. Returns none if the session is not in the catalog or 'shouldKill' declines it.
     *
     * 'shouldKill' runs with the session's partition locked, so the same restrictions as
     * 'scanSession' apply to it: no blocking, no I/O and no lock manager locks. The token is only
     * handed back once that partition has been released, so that returning the kill cannot deadlock
     * against it.
     */
    boost::optional<KillToken> killSessionIf(const LogicalSessionId& lsid,
                                             const KillSessionsPredicateFn& shouldKill,
                                             ErrorCodes::Error reason = ErrorCodes::Interrupted);

    /**
     * Returns the total number of entries currently cached on the session catalog. Takes no
     * partition mutex, so it is safe to call from diagnostic paths such as FTDC during a scan.
     */
    size_t size() const;

    /**
     * Returns the number of sessions with a kill which has been requested but neither consumed by
     * 'checkOutSessionForKill' nor returned.
     */
    size_t numSessionsWithOutstandingKills() const;

    size_t numPartitions_forTest() const {
        return _partitions.size();
    }

    /**
     * Registers two callbacks: one to run when sessions are "eagerly" reaped from the catalog, ie
     * without waiting for a logical session cache refresh, and another to override the logic that
     * determines when to eagerly reap a session.
     */
    void setEagerReapSessionsFns(OnEagerlyReapedSessionsFn onEagerlyReapedSessionsFn,
                                 MakeSessionWorkerFnForEagerReap makeWorkerFnForEagerReap) {
        invariant(!_onEagerlyReapedSessionsFn);
        _onEagerlyReapedSessionsFn = std::move(onEagerlyReapedSessionsFn);
        _makeSessionWorkerFnForEagerReap = std::move(makeWorkerFnForEagerReap);
    }

    /**
     * Called on shutdown to prevent the TransactionRouter from starting a new transaction.
     */
    void setDisallowNewTransactions();
    bool getDisallowNewTransactions();

private:
    /**
     * Tracks the runtime info for transaction sessions that corresponds to the same logical
     * session. Designed such that only one transaction session can be checked out at any given
     * time.
     */
    struct SessionRuntimeInfo {
        SessionRuntimeInfo(SessionCatalog* catalog, LogicalSessionId lsid)
            : catalog(catalog), parentSession(std::move(lsid)) {
            // Can only create a SessionRuntimeInfo with a parent transaction session id.
            invariant(isParentSessionId(parentSession.getSessionId()));
        }

        Session* getSession(WithLock, const LogicalSessionId& lsid);

        /**
         * Retires one outstanding kill and wakes up anybody waiting to check the session out. The
         * only place 'killsRequested' is decremented.
         */
        void returnKill(WithLock);

        // The catalog owning this session. Never changes and is never null.
        SessionCatalog* const catalog;

        // Must only be accessed by the OperationContext which currently has this logical session
        // checked out.
        Session parentSession;
        LogicalSessionIdMap<Session> childSessions;

        // The latest client txnNumber that has successfully started running on this logical
        // session. This is set to kUninitializedTxnNumber initially, and is updated every time an
        // opCtx that starts a new client txnNumber checks this logical session back in.
        TxnNumber lastClientTxnNumberStarted = kUninitializedTxnNumber;

        // Signaled when the state becomes available. Uses the owning catalog partition's mutex to
        // protect the state transitions.
        stdx::condition_variable availableCondVar;

        // Pointer to the OperationContext for the operation running on this logical session, or
        // nullptr if there is no operation currently running on the session.
        OperationContext* checkoutOpCtx{nullptr};

        // Last check-out time for this logical session. Updated every time any of the transaction
        // sessions gets checked out.
        Date_t lastCheckout{Date_t::now()};

        // Counter indicating the number of times ObservableSession::kill has been called on this
        // SessionRuntimeInfo, which have not yet had a corresponding call to
        // checkOutSessionForKill.
        int killsRequested{0};
    };
    using SessionRuntimeInfoMap = LogicalSessionIdMap<std::unique_ptr<SessionRuntimeInfo>>;

    /**
     * A shard of the session catalog. Each partition owns the sessions whose parent lsid hashes to
     * it; the partition's mutex protects its map, the runtime state of the sessions it owns, and
     * serves as the mutex for their 'availableCondVar' waits. Parent and child sessions share the
     * lsid 'id' component, so a whole session family always lives in a single partition.
     *
     * Aligned so that no two partitions share a cache line: ObservableMutex writes its counters on
     * every lock, including uncontended ones.
     */
    class alignas(std::hardware_destructive_interference_size) Partition {
    public:
        /**
         * Locks the partition and is the only way to reach its session map. Instantiate through
         * the Locked or ConstLocked aliases.
         */
        template <typename P>
        class LockedImpl {
        public:
            explicit LockedImpl(P& partition) : _lock(partition._mutex), _partition(&partition) {
                _prevHeld = std::exchange(session_catalog_detail::heldPartition(), &partition);
            }

            ~LockedImpl() {
                session_catalog_detail::heldPartition() = _prevHeld;
            }

            LockedImpl(LockedImpl&&) = delete;
            LockedImpl& operator=(LockedImpl&&) = delete;

            auto& sessions() const {
                return _partition->_sessions;
            }

            auto& lock() {
                return _lock;
            }

            explicit(false) operator WithLock() const {
                return WithLock(_lock);
            }

        private:
            std::unique_lock<ObservableMutex<std::mutex>> _lock;
            P* _partition;
            const void* _prevHeld;
        };
        using Locked = LockedImpl<Partition>;
        using ConstLocked = LockedImpl<const Partition>;

        const ObservableMutex<std::mutex>& mutex() const {
            return _mutex;
        }

        /**
         * Whether this partition is locked by the calling thread. Only ever used to catch a lock
         * which is about to be taken recursively, so a stale 'false' just means the check is
         * skipped.
         */
        bool isLockedByCurrentThread() const {
            return session_catalog_detail::heldPartition() == static_cast<const void*>(this);
        }

    private:
        mutable ObservableMutex<std::mutex> _mutex;
        SessionRuntimeInfoMap _sessions;
    };

    /**
     * Locks and returns the partition owning 'lsid'.
     */
    Partition::Locked _lockPartition(const LogicalSessionId& lsid);

    /**
     * Returns a callback with the default logic used to decide if a session may be reaped early.
     */
    static ScanSessionsCallbackFn _defaultMakeSessionWorkerFnForEagerReap(
        ServiceContext* service, TxnNumber clientTxnNumberStarted, Provenance provenance);

    /**
     * Blocking method, which checks-out the session with the given 'lsid'. Called inside
     * '_checkOutSession' and 'checkOutSessionForKill'.
     */
    ScopedCheckedOutSession _checkOutSessionInner(OperationContext* opCtx,
                                                  const LogicalSessionId& lsid,
                                                  boost::optional<KillToken> killToken,
                                                  Date_t deadline = Date_t::max());

    /**
     * Blocking method, which checks-out the session set on 'opCtx'.
     */
    ScopedCheckedOutSession _checkOutSession(OperationContext* opCtx);

    /**
     * Returns the session runtime info for 'lsid', or nullptr. The returned pointer stays linked
     * on the map for as long as the partition stays locked.
     */
    SessionRuntimeInfo* _getSessionRuntimeInfo(const Partition::Locked& partition,
                                               const LogicalSessionId& lsid);

    /**
     * Creates or returns the session runtime info for 'lsid'. The returned pointer stays linked on
     * the map for as long as the partition stays locked.
     */
    SessionRuntimeInfo* _getOrCreateSessionRuntimeInfo(const Partition::Locked& partition,
                                                       const LogicalSessionId& lsid);

    /**
     * Retires the kill 'killToken' holds and disarms it, so it cannot be returned again. The
     * overload taking a lock is for callers which already hold the session's partition.
     */
    void _returnKill(WithLock, SessionRuntimeInfo* sri, KillToken& killToken);
    void _returnKill(KillToken& killToken);

    /**
     * Returns the partition owning 'lsid', without locking it.
     */
    Partition& _partitionFor(const LogicalSessionId& lsid);

    /**
     * Makes a session, previously checked out through 'checkoutSession', available again. Will free
     * any retryable sessions with txnNumbers before clientTxnNumberStarted if it is set.
     */
    void _releaseSession(SessionRuntimeInfo* sri,
                         Session* session,
                         boost::optional<KillToken> killToken,
                         boost::optional<TxnNumberAndProvenance> clientTxnNumberStarted);

    // Called when sessions are reaped from memory "eagerly" ie directly by the SessionCatalog
    // without waiting for a logical session cache refresh. Note this is set at process startup
    // before multi-threading is enabled, so no synchronization is necessary.
    boost::optional<OnEagerlyReapedSessionsFn> _onEagerlyReapedSessionsFn;

    // Returns a callback used to decide if a session may be "eagerly" reaped from the session
    // catalog without waiting for typical logical session expiration. May be overwritten, but only
    // at process startup before multi-threading is enabled, so no synchronization is necessary.
    MakeSessionWorkerFnForEagerReap _makeSessionWorkerFnForEagerReap =
        _defaultMakeSessionWorkerFnForEagerReap;

    // Owns the Session objects for all current Sessions, sharded by parent lsid. Sized at
    // construction from 'sessionCatalogPartitions' and never resized.
    std::vector<Partition> _partitions;

    // Number of entries across all partitions, so that size() takes no partition mutex.
    Atomic<long long> _numParentSessions{0};

    Atomic<bool> _disallowNewTransactions{false};

    // Number of sessions whose 'killsRequested' is above zero, so that it can be reported without
    // locking any partition.
    Atomic<int> _numSessionsWithOutstandingKills{0};
};

/**
 * Scoped object representing a checked-out transaction session. This type is an implementation
 * detail of the SessionCatalog.
 */
class SessionCatalog::ScopedCheckedOutSession {
public:
    ScopedCheckedOutSession(SessionCatalog& catalog,
                            SessionCatalog::SessionRuntimeInfo* sri,
                            Session* session,
                            boost::optional<SessionCatalog::KillToken> killToken)
        : _catalog(catalog), _sri(sri), _session(session), _killToken(std::move(killToken)) {
        if (_killToken) {
            invariant(session->getSessionId() == _killToken->lsidToKill());
        }
    }

    ScopedCheckedOutSession(ScopedCheckedOutSession&& other) noexcept
        : _catalog(other._catalog),
          _clientTxnNumberStartedAndProvenance(
              std::move(other._clientTxnNumberStartedAndProvenance)),
          _sri(other._sri),
          _session(other._session),
          _killToken(std::move(other._killToken)) {
        other._sri = nullptr;
    }

    ScopedCheckedOutSession& operator=(ScopedCheckedOutSession&&) = delete;
    ScopedCheckedOutSession(const ScopedCheckedOutSession&) = delete;
    ScopedCheckedOutSession& operator=(ScopedCheckedOutSession&) = delete;

    ~ScopedCheckedOutSession() {
        if (_sri) {
            _catalog._releaseSession(
                _sri, _session, std::move(_killToken), _clientTxnNumberStartedAndProvenance);
        }
    }

    OperationContext* currentOperation_forTest() const {
        return _sri->checkoutOpCtx;
    }

    Session* get() const {
        return _session;
    }

    Session* operator->() const {
        return get();
    }

    Session& operator*() const {
        return *get();
    }

    bool wasCheckedOutForKill() const {
        return bool(_killToken);
    }

    void observeNewClientTxnNumberStarted(
        SessionCatalog::TxnNumberAndProvenance txnNumberAndProvenance) {
        _clientTxnNumberStartedAndProvenance = txnNumberAndProvenance;
    }

private:
    // The owning session catalog into which the session should be checked back
    SessionCatalog& _catalog;

    // If this session began a retryable write or transaction while checked out, this is set to the
    // "client txnNumber" of that transaction, which is the top-level txnNumber for a retryable
    // write or transaction sent by a client or the txnNumber in the sessionId for a retryable
    // child transaction, and the "provenance" of the number, ie whether the number came from the
    // router or participant role.
    boost::optional<SessionCatalog::TxnNumberAndProvenance> _clientTxnNumberStartedAndProvenance;

    SessionCatalog::SessionRuntimeInfo* _sri;
    Session* _session;
    boost::optional<SessionCatalog::KillToken> _killToken;
};

class OperationContextSession;

/**
 * RAII type returned by SessionCatalog::checkOutSessionForKill.
 *
 * After calling kill() on an ObservableSession, let that ObservableSession go out
 * of scope and in a context outside of SessionCatalog::killSessions, call checkOutSessionForKill
 * to get an instance of this type. Then, while holding that instance, perform any cleanup
 * you need to perform on a session as part of killing it. More details in the description of
 * ObservableSession::kill, below.
 */
class SessionCatalog::SessionToKill {
public:
    SessionToKill(ScopedCheckedOutSession&& scos) : _scos(std::move(scos)) {}

    Session* get() const {
        return _scos.get();
    }

    const LogicalSessionId& getSessionId() const {
        return get()->getSessionId();
    }

    OperationContext* currentOperation_forTest() const {
        return _scos.currentOperation_forTest();
    }

private:
    friend OperationContextSession;

    ScopedCheckedOutSession _scos;
};
using SessionToKill = SessionCatalog::SessionToKill;

/**
 * This type represents access to a transaction session inside of a SessionCatalog scan.
 * If you have one of these, you're in a scan callback context, and so
 * have locked the catalog partition owning the observed session and, if the observed session is
 * bound to an operation context, you hold that operation context's client's mutex, as well.
 */
class [[MONGO_MOD_PUBLIC]] ObservableSession {
public:
    ObservableSession(const ObservableSession&) = delete;
    ObservableSession(ObservableSession&&) = delete;
    ObservableSession& operator=(const ObservableSession&) = delete;
    ObservableSession& operator=(ObservableSession&&) = delete;

    /**
     * The session id for this transaction session.
     */
    const LogicalSessionId& getSessionId() const {
        return _session->_sessionId;
    }

    /**
     * The latest client txnNumber that has successfully started running on the logical session that
     * this transaction session corresponds to.
     */
    TxnNumber getLastClientTxnNumberStarted() const {
        return _sri->lastClientTxnNumberStarted;
    }

    /**
     * Returns true if there is an operation currently running on the logical session that this
     * transaction session corresponds to.
     */
    bool hasCurrentOperation() const {
        return _sri->checkoutOpCtx;
    }

    /**
     * Returns the last check-out time for the logical session that this transaction session
     * corresponds to. Used for reaping purposes.
     */
    Date_t getLastCheckout() const {
        return _sri->lastCheckout;
    }


    /**
     * To be used with 'scanSessionsForReap' to indicate to the SessionCatalog that, from the user
     * perspective, this transaction session is safe to be reaped. That is, the reaper has checked
     * that the session has expired and all the decorations they are using are prepared to be
     * destroyed. There are two reap modes:
     * - kExclusive indicates that the session is safe to be reaped independently of the other
     *   sessions matched by 'scanSessionsForReap'.
     * - kNonExclusive indicates that the session is only safe to reaped if all the other sessions
     *   are also safe to be reaped.
     *
     * Calling this method does not guarantee that the session will in fact be reaped. The
     * SessionCatalog performs additional checks to protect sessions that are still in use from
     * being reaped. However, reaping will still obey the specified reap mode. See the comment for
     * '_shouldBeReaped' for more info.
     */
    enum class [[MONGO_MOD_PRIVATE]] ReapMode { kExclusive, kNonExclusive };
    [[MONGO_MOD_PRIVATE]] void markForReap(ReapMode reapMode);

    /**
     * Returns the OperationContext that currently has this session checked out, or nullptr if the
     * session is not checked out. Safe to call inside a scanSessions callback because the catalog
     * mutex and (if applicable) the client mutex are already held.
     */
    OperationContext* currentOperationContext() const {
        return _sri->checkoutOpCtx;
    }

    /**
     * Returns a pointer to the Session itself.
     */
    Session* get() const {
        return _session;
    }

private:
    friend class SessionCatalog;

    /**
     * Marks the session as killed, interrupting whichever operation has it checked out, and returns
     * a token for 'checkOutSessionForKill'. Check-outs block until every token is gone.
     *
     * Private because it runs with the partition locked while returning a kill locks it again, so
     * only the catalog, which hands tokens out after unlocking, may call it. See 'killSessionIf'.
     */
    SessionCatalog::KillToken kill(ErrorCodes::Error reason = ErrorCodes::Interrupted) const;

    static ClientLock _lockClientForSession(WithLock, SessionCatalog::SessionRuntimeInfo* sri) {
        if (const auto opCtx = sri->checkoutOpCtx) {
            return ClientLock{opCtx->getClient()};
        }
        return {};
    }

    ObservableSession(WithLock wl, SessionCatalog::SessionRuntimeInfo* sri, Session* session)
        : _sri(sri), _session(session), _clientLock(_lockClientForSession(std::move(wl), _sri)) {}

    /**
     * Returns whether 'kill' has been called on this session.
     */
    bool _killed() const;

    /**
     * Returns true if this Session can be checked out.
     */
    bool _isAvailableForCheckOut(bool forKill) const {
        return !hasCurrentOperation() && (forKill || !_killed());
    }

    /**
     * Returns true if this transaction session should be be reaped from the SessionCatalog.
     * That is, the session has been marked for reap and both of the following are true:
     * - It is not checked out by any thread, and there are no threads waiting for it to be
     *   checked out.
     * - It is not marked for kill (i.e. expected to be checked out for kill).
     */
    bool _shouldBeReaped() const;

    SessionCatalog::SessionRuntimeInfo* _sri;
    Session* _session;
    mutable ClientLock _clientLock;

    bool _markedForReap{false};
    boost::optional<ReapMode> _reapMode;
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction.
 */
class [[MONGO_MOD_PUBLIC]] OperationContextSession {
    OperationContextSession(const OperationContextSession&) = delete;
    OperationContextSession& operator=(const OperationContextSession&) = delete;

public:
    /**
     * Acquires the session with id opCtx->getLogicalSessionId().  Because a session can only be
     * checked out by one user at a time, construction of OperationContextSession can block waiting
     * for the desired session to be checked in by another user.
     */
    OperationContextSession(OperationContext* opCtx);

    /**
     * Same as constructor above, but takes the session id from the killToken and uses
     * checkoutSessionForKill instead, placing the checked-out session on the operation context.
     * Must not be called if the operation context contains a session.
     */
    OperationContextSession(OperationContext* opCtx, SessionCatalog::KillToken killToken);
    ~OperationContextSession();

    /**
     * Returns the session currently checked out by "opCtx", or nullptr if the opCtx has no
     * checked out session.
     */
    static Session* get(OperationContext* opCtx);

    /**
     * These methods take an operation context with a checked-out session and allow it to be
     * temporarily or permanently checked back in, in order to allow other operations to use it.
     *
     * Check-in may only be called if the session has actually been checked out previously and
     * similarly check-out may only be called if the session is not checked out already.
     */
    enum class CheckInReason { kDone, kYield };
    static void checkIn(OperationContext* opCtx, CheckInReason reason);
    static void checkOut(OperationContext* opCtx);

    /**
     * Notifies the session catalog when a new transaction/retryable write is begun on the operation
     * context's checked out session.
     */
    static void observeNewTxnNumberStarted(
        OperationContext* opCtx,
        const LogicalSessionId& lsid,
        SessionCatalog::TxnNumberAndProvenance txnNumberAndProvenance);

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
