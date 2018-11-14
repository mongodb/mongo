
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/session.h"
#include "mongo/db/session_killer.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class ScopedSession;
class ScopedCheckedOutSession;
class ServiceContext;

/**
 * Keeps track of the transaction runtime state for every active session on this instance.
 */
class SessionCatalog {
    MONGO_DISALLOW_COPYING(SessionCatalog);

    friend class ScopedSession;
    friend class ScopedCheckedOutSession;

public:
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
     * Potentially blocking call, which uses the session information stored in the specified
     * operation context and either creates a brand new session object (if one doesn't exist) or
     * "checks-out" the existing one (if it is not currently in use or marked for kill).
     *
     * Checking out a session puts it in the 'checked out' state and all subsequent calls to
     * checkout will block until it is checked back in. This happens when the returned object goes
     * out of scope.
     *
     * Throws exception on errors.
     */
    ScopedCheckedOutSession checkOutSession(OperationContext* opCtx);

    /**
     * See the description of 'Session::kill' for more information on the session kill usage
     * pattern.
     */
    ScopedCheckedOutSession checkOutSessionForKill(OperationContext* opCtx,
                                                   Session::KillToken killToken);

    /**
     * Returns a reference to the specified cached session regardless of whether it is checked-out
     * or not. The returned session is not returned checked-out and is allowed to be checked-out
     * concurrently.
     *
     * The intended usage for this method is to allow migrations to run in parallel with writes for
     * the same session without blocking it. Because of this, it may not be used from operations
     * which run on a session.
     */
    ScopedSession getOrCreateSession(OperationContext* opCtx, const LogicalSessionId& lsid);

    /**
     * Iterates through the SessionCatalog under the SessionCatalog mutex and applies 'workerFn' to
     * each Session which matches the specified 'matcher'.
     *
     * NOTE: Since this method runs with the session catalog mutex, the work done by 'workerFn' is
     * not allowed to block, perform I/O or acquire any lock manager locks.
     * Iterates through the SessionCatalog and applies 'workerFn' to each Session. This locks the
     * SessionCatalog.
     *
     * TODO SERVER-33850: Take Matcher out of the SessionKiller namespace.
     */
    using ScanSessionsCallbackFn = stdx::function<void(WithLock, Session*)>;
    void scanSessions(const SessionKiller::Matcher& matcher,
                      const ScanSessionsCallbackFn& workerFn);

    /**
     * Shortcut to invoke 'kill' on the specified session under the SessionCatalog mutex. Throws a
     * NoSuchSession exception if the session doesn't exist.
     */
    Session::KillToken killSession(const LogicalSessionId& lsid);

private:
    struct SessionRuntimeInfo {
        SessionRuntimeInfo(LogicalSessionId lsid) : session(std::move(lsid)) {}

        // Must only be accessed when the state is kInUse and only by the operation context, which
        // currently has it checked out
        Session session;

        // Signaled when the state becomes available. Uses the transaction table's mutex to protect
        // the state transitions.
        stdx::condition_variable availableCondVar;
    };

    /**
     * May release and re-acquire it zero or more times before returning. The returned
     * 'SessionRuntimeInfo' is guaranteed to be linked on the catalog's _txnTable as long as the
     * lock is held.
     */
    std::shared_ptr<SessionRuntimeInfo> _getOrCreateSessionRuntimeInfo(
        WithLock, OperationContext* opCtx, const LogicalSessionId& lsid);

    /**
     * Makes a session, previously checked out through 'checkoutSession', available again.
     */
    void _releaseSession(const LogicalSessionId& lsid,
                         boost::optional<Session::KillToken> killToken);

    stdx::mutex _mutex;

    // Owns the Session objects for all current Sessions.
    LogicalSessionIdMap<std::shared_ptr<SessionRuntimeInfo>> _sessions;
};

/**
 * Scoped object representing a reference to a session.
 */
class ScopedSession {
public:
    explicit ScopedSession(std::shared_ptr<SessionCatalog::SessionRuntimeInfo> sri)
        : _sri(std::move(sri)) {
        invariant(_sri);
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

    operator bool() const {
        return !!_sri;
    }

private:
    std::shared_ptr<SessionCatalog::SessionRuntimeInfo> _sri;
};

/**
 * Scoped object representing a checked-out session. See comments for the 'checkoutSession' method
 * for more information on its behaviour.
 */
class ScopedCheckedOutSession {
    MONGO_DISALLOW_COPYING(ScopedCheckedOutSession);

    friend ScopedCheckedOutSession SessionCatalog::checkOutSession(OperationContext*);
    friend ScopedCheckedOutSession SessionCatalog::checkOutSessionForKill(OperationContext*,
                                                                          Session::KillToken);

public:
    ScopedCheckedOutSession(ScopedCheckedOutSession&&) = default;

    ~ScopedCheckedOutSession() {
        if (_scopedSession) {
            SessionCatalog::get(_opCtx)->_releaseSession(_scopedSession->getSessionId(),
                                                         std::move(_killToken));
        }
    }

    Session* get() const {
        return _scopedSession.get();
    }

    Session* operator->() const {
        return get();
    }

    Session& operator*() const {
        return *get();
    }

    operator bool() const {
        return _scopedSession;
    }

private:
    ScopedCheckedOutSession(OperationContext* opCtx,
                            ScopedSession scopedSession,
                            boost::optional<Session::KillToken> killToken)
        : _opCtx(opCtx),
          _killToken(std::move(killToken)),
          _scopedSession(std::move(scopedSession)) {}

    OperationContext* const _opCtx;

    boost::optional<Session::KillToken> _killToken;

    ScopedSession _scopedSession;
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction.
 */
class OperationContextSession {
    MONGO_DISALLOW_COPYING(OperationContextSession);

public:
    OperationContextSession(OperationContext* opCtx);
    ~OperationContextSession();

    /**
     * Returns the session checked out in the constructor.
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
