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
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ObservableSession;

/**
 * Keeps track of the transaction runtime state for every active transaction session on this
 * instance.
 */
class SessionCatalog {
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
    using OnEagerlyReapedSessionsFn =
        unique_function<void(ServiceContext*, std::vector<LogicalSessionId>)>;
    using MakeSessionWorkerFnForEagerReap =
        unique_function<ScanSessionsCallbackFn(ServiceContext*, TxnNumber, Provenance)>;

    class ScopedCheckedOutSession;
    class SessionToKill;

    struct KillToken {
        KillToken(LogicalSessionId lsid) : lsidToKill(std::move(lsid)) {}
        KillToken(KillToken&&) = default;
        KillToken& operator=(KillToken&&) = default;

        LogicalSessionId lsidToKill;
    };

    SessionCatalog() = default;
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
    void reset_forTest();

    /**
     * See the description of 'ObservableSession::kill' for more information on the session kill
     * usage pattern.
     */
    SessionToKill checkOutSessionForKill(OperationContext* opCtx,
                                         KillToken killToken,
                                         Milliseconds* timeout = nullptr);

    /**
     * Iterates through the SessionCatalog under the SessionCatalog mutex and applies 'workerFn' to
     * each Session which matches the specified 'lsid' or 'matcher'. Does not support reaping.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by 'workerFn' is
     * not allowed to block, perform I/O or acquire any lock manager locks.
     * Iterates through the SessionCatalog and applies 'workerFn' to each Session. This locks the
     * SessionCatalog.
     */
    enum class ScanSessionCreateSession { kYes, kNo };
    void scanSession(const LogicalSessionId& lsid,
                     const ScanSessionsCallbackFn& workerFn,
                     ScanSessionCreateSession createSession = ScanSessionCreateSession::kNo);
    void scanSessions(const SessionKiller::Matcher& matcher,
                      const ScanSessionsCallbackFn& workerFn);

    /**
     * Same as the above but only applies 'workerFn' to parent Sessions.
     */
    void scanParentSessions(const ScanSessionsCallbackFn& workerFn);

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
     * Returns the total number of entries currently cached on the session catalog.
     */
    size_t size() const;

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
        SessionRuntimeInfo(LogicalSessionId lsid) : parentSession(std::move(lsid)) {
            // Can only create a SessionRuntimeInfo with a parent transaction session id.
            invariant(isParentSessionId(parentSession.getSessionId()));
        }

        Session* getSession(WithLock, const LogicalSessionId& lsid);

        // Must only be accessed by the OperationContext which currently has this logical session
        // checked out.
        Session parentSession;
        LogicalSessionIdMap<Session> childSessions;

        // The latest client txnNumber that has successfully started running on this logical
        // session. This is set to kUninitializedTxnNumber initially, and is updated every time an
        // opCtx that starts a new client txnNumber checks this logical session back in.
        TxnNumber lastClientTxnNumberStarted = kUninitializedTxnNumber;

        // Signaled when the state becomes available. Uses the transaction table's mutex to protect
        // the state transitions.
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
                                                  Milliseconds* timeout = nullptr);

    /**
     * Blocking method, which checks-out the session set on 'opCtx'.
     */
    ScopedCheckedOutSession _checkOutSession(OperationContext* opCtx);

    /**
     * Returns the session runtime info for 'lsid' from the '_sessions' map. The returned pointer
     * is guaranteed to be linked on the map for as long as the mutex is held.
     */
    SessionRuntimeInfo* _getSessionRuntimeInfo(WithLock lk, const LogicalSessionId& lsid);

    /**
     * Creates or returns the session runtime info for 'lsid' from the '_sessions' map. The
     * returned pointer is guaranteed to be linked on the map for as long as the mutex is held.
     */
    SessionRuntimeInfo* _getOrCreateSessionRuntimeInfo(WithLock lk, const LogicalSessionId& lsid);

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

    // Protects the state below
    mutable stdx::mutex _mutex;

    // Owns the Session objects for all current Sessions.
    SessionRuntimeInfoMap _sessions;

    AtomicWord<bool> _disallowNewTransactions{false};
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
            invariant(session->getSessionId() == _killToken->lsidToKill);
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
 * of scope and in a context outside of SessionCatalog::scanSessions, call checkOutSessionForKill
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
 * This type represents access to a transaction session inside of a scanSessions loop.
 * If you have one of these, you're in a scanSessions callback context, and so
 * have locked the whole catalog and, if the observed session is bound to an operation context,
 * you hold that operation context's client's mutex, as well.
 */
class ObservableSession {
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
     * Increments the number of "killers" for the logical session that this transaction session
     * corresponds to and returns a 'kill token' to to be passed later on to
     * 'checkOutSessionForKill' method of the SessionCatalog in order to permit the caller to
     * execute any kill cleanup tasks. This token is later used to decrement the number of
     * "killers".
     *
     * Marking session as killed is an internal property only that will cause any further calls to
     * 'checkOutSession' to block until 'checkOutSessionForKill' is called the same number of times
     * as 'kill' was called and the returned scoped object destroyed.
     *
     * If the first killer finds the session checked-out, this method will also interrupt the
     * operation context which has it checked-out.
     */
    SessionCatalog::KillToken kill(ErrorCodes::Error reason = ErrorCodes::Interrupted) const;

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
    enum class ReapMode { kExclusive, kNonExclusive };
    void markForReap(ReapMode reapMode);

    /**
     * Returns a pointer to the Session itself.
     */
    Session* get() const {
        return _session;
    }

private:
    friend class SessionCatalog;

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
class OperationContextSession {
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
