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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session.h"
#include "mongo/db/session_killer.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class ObservableSession;

/**
 * Keeps track of the transaction runtime state for every active session on this instance.
 */
class SessionCatalog {
    SessionCatalog(const SessionCatalog&) = delete;
    SessionCatalog& operator=(const SessionCatalog&) = delete;

    friend class ObservableSession;
    friend class OperationContextSession;

public:
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
    SessionToKill checkOutSessionForKill(OperationContext* opCtx, KillToken killToken);

    /**
     * Iterates through the SessionCatalog under the SessionCatalog mutex and applies 'workerFn' to
     * each Session which matches the specified 'matcher'.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by 'workerFn' is
     * not allowed to block, perform I/O or acquire any lock manager locks.
     * Iterates through the SessionCatalog and applies 'workerFn' to each Session. This locks the
     * SessionCatalog.
     */
    using ScanSessionsCallbackFn = std::function<void(ObservableSession&)>;
    void scanSession(const LogicalSessionId& lsid, const ScanSessionsCallbackFn& workerFn);
    void scanSessions(const SessionKiller::Matcher& matcher,
                      const ScanSessionsCallbackFn& workerFn);

    /**
     * Shortcut to invoke 'kill' on the specified session under the SessionCatalog mutex. Throws a
     * NoSuchSession exception if the session doesn't exist.
     */
    KillToken killSession(const LogicalSessionId& lsid);

    /**
     * Returns the total number of entries currently cached on the session catalog.
     */
    size_t size() const;

private:
    struct SessionRuntimeInfo {
        SessionRuntimeInfo(LogicalSessionId lsid) : session(std::move(lsid)) {}
        ~SessionRuntimeInfo();

        // Must only be accessed when the state is kInUse and only by the operation context, which
        // currently has it checked out
        Session session;

        // Counts how many threads have called checkOutSession/checkOutSessionForKill and are
        // blocked in it waiting for the session to become available. Used to block reaping of
        // sessions entries from the map.
        int numWaitingToCheckOut{0};

        // Signaled when the state becomes available. Uses the transaction table's mutex to protect
        // the state transitions.
        stdx::condition_variable availableCondVar;
    };
    using SessionRuntimeInfoMap = LogicalSessionIdMap<std::unique_ptr<SessionRuntimeInfo>>;

    /**
     * Blocking method, which checks-out the session set on 'opCtx'.
     */
    ScopedCheckedOutSession _checkOutSession(OperationContext* opCtx);

    /**
     * Creates or returns the session runtime info for 'lsid' from the '_sessions' map. The returned
     * pointer is guaranteed to be linked on the map for as long as the mutex is held.
     */
    SessionRuntimeInfo* _getOrCreateSessionRuntimeInfo(WithLock,
                                                       OperationContext* opCtx,
                                                       const LogicalSessionId& lsid);

    /**
     * Makes a session, previously checked out through 'checkoutSession', available again.
     */
    void _releaseSession(SessionRuntimeInfo* sri, boost::optional<KillToken> killToken);

    // Protects the state below
    mutable stdx::mutex _mutex;

    // Owns the Session objects for all current Sessions.
    SessionRuntimeInfoMap _sessions;
};

/**
 * Scoped object representing a checked-out session. This type is an implementation detail
 * of the SessionCatalog.
 */
class SessionCatalog::ScopedCheckedOutSession {
public:
    ScopedCheckedOutSession(SessionCatalog& catalog,
                            SessionCatalog::SessionRuntimeInfo* sri,
                            boost::optional<SessionCatalog::KillToken> killToken)
        : _catalog(catalog), _sri(sri), _killToken(std::move(killToken)) {}

    ScopedCheckedOutSession(ScopedCheckedOutSession&& other)
        : _catalog(other._catalog), _sri(other._sri), _killToken(std::move(other._killToken)) {
        other._sri = nullptr;
    }

    ScopedCheckedOutSession& operator=(ScopedCheckedOutSession&&) = delete;
    ScopedCheckedOutSession(const ScopedCheckedOutSession&) = delete;
    ScopedCheckedOutSession& operator=(ScopedCheckedOutSession&) = delete;

    ~ScopedCheckedOutSession() {
        if (_sri) {
            _catalog._releaseSession(_sri, std::move(_killToken));
        }
    }

    Session* get() const {
        return &_sri->session;
    }

    Session* operator->() const {
        return get();
    }

    Session& operator*() const {
        return *get();
    }

private:
    // The owning session catalog into which the session should be checked back
    SessionCatalog& _catalog;

    SessionCatalog::SessionRuntimeInfo* _sri;
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
        return get()->currentOperation_forTest();
    }

private:
    friend OperationContextSession;

    ScopedCheckedOutSession _scos;
};
using SessionToKill = SessionCatalog::SessionToKill;

/**
 * This type represents access to a session inside of a scanSessions loop.
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
     * The logical session id that this object represents.
     */
    const LogicalSessionId& getSessionId() const {
        return _session->_sessionId;
    }

    /**
     * Returns a pointer to the current operation running on this Session, or nullptr if there is no
     * operation currently running on this Session.
     */
    OperationContext* currentOperation() const {
        return _session->_checkoutOpCtx;
    }

    /**
     * Returns when is the last time this session was checked-out, for reaping purposes.
     */
    Date_t getLastCheckout() const {
        return _session->_lastCheckout;
    }

    /**
     * Increments the number of "killers" for this session and returns a 'kill token' to to be
     * passed later on to 'checkOutSessionForKill' method of the SessionCatalog in order to permit
     * the caller to execute any kill cleanup tasks. This token is later used to decrement the
     * number of "killers".
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
     * Indicates to the SessionCatalog that the session tracked by this object is safe to be deleted
     * from the map. It is up to the caller to provide the necessary checks that all the decorations
     * they are using are prepared to be destroyed.
     *
     * Calling this method does not guarantee that the session will in fact be destroyed, which
     * could happen if there are threads waiting for it to be checked-out.
     */
    void markForReap();

    /**
     * Returns a pointer to the Session itself.
     */
    Session* get() const {
        return _session;
    }

private:
    friend class SessionCatalog;

    static stdx::unique_lock<Client> _lockClientForSession(WithLock, Session* session) {
        if (const auto opCtx = session->_checkoutOpCtx) {
            return stdx::unique_lock<Client>{*opCtx->getClient()};
        }
        return {};
    }

    ObservableSession(WithLock wl, Session& session)
        : _session(&session), _clientLock(_lockClientForSession(std::move(wl), _session)) {}

    /**
     * Returns whether 'kill' has been called on this session.
     */
    bool _killed() const;

    Session* _session;
    stdx::unique_lock<Client> _clientLock;
    bool _markedForReap{false};
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
    static void checkIn(OperationContext* opCtx);
    static void checkOut(OperationContext* opCtx);

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
