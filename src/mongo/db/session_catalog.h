/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/session.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

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
    explicit SessionCatalog(ServiceContext* serviceContext);
    ~SessionCatalog();

    /**
     * Instantiates a transaction table on the specified service context. Must be called only once
     * and is not thread-safe.
     */
    static void create(ServiceContext* service);

    /**
     * Resets the transaction table on the specified service context to an uninitialized state.
     * Meant only for testing.
     */
    static void reset_forTest(ServiceContext* service);

    /**
     * Retrieves the session transaction table associated with the service or operation context.
     * Must only be called after 'create' has been called.
     */
    static SessionCatalog* get(OperationContext* opCtx);
    static SessionCatalog* get(ServiceContext* service);

    /**
     * Fetches the UUID of the transaction table, or an empty optional if the collection does not
     * exist or has no UUID. Acquires a lock on the collection. Required for rollback via refetch.
     */
    static boost::optional<UUID> getTransactionTableUUID(OperationContext* opCtx);

    /**
     * Invoked when the node enters the primary state. Ensures that the transactions collection is
     * created. Throws on severe exceptions due to which it is not safe to continue the step-up
     * process.
     */
    void onStepUp(OperationContext* opCtx);

    /**
     * Potentially blocking call, which uses the session information stored in the specified
     * operation context and either creates a new session runtime state (if one doesn't exist) or
     * "checks-out" the existing one (if it is not currently in use).
     *
     * Checking out a session puts it in the 'in use' state and all subsequent calls to checkout
     * will block until it is put back in the 'available' state when the returned object goes out of
     * scope.
     *
     * Throws exception on errors.
     */
    ScopedCheckedOutSession checkOutSession(OperationContext* opCtx);

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
     * Callback to be invoked when it is suspected that the on-disk session contents might not be in
     * sync with what is in the sessions cache.
     *
     * If no specific document is available, the method will invalidate all sessions. Otherwise if
     * one is avaiable (which is the case for insert/update/delete), it must contain _id field with
     * a valid session entry, in which case only that particular session will be invalidated. If the
     * _id field is missing or doesn't contain a valid serialization of logical session, the method
     * will throw. This prevents invalid entries from making it in the collection.
     */
    void invalidateSessions(OperationContext* opCtx, boost::optional<BSONObj> singleSessionDoc);

private:
    struct SessionRuntimeInfo {
        SessionRuntimeInfo(LogicalSessionId lsid) : txnState(std::move(lsid)) {}

        // Current check-out state of the session. If set to false, the session can be checked out.
        // If set to true, the session is in use by another operation and the caller must wait to
        // check it out.
        bool checkedOut{false};

        // Signaled when the state becomes available. Uses the transaction table's mutex to protect
        // the state transitions.
        stdx::condition_variable availableCondVar;

        // Must only be accessed when the state is kInUse and only by the operation context, which
        // currently has it checked out
        Session txnState;
    };

    using SessionRuntimeInfoMap = stdx::unordered_map<LogicalSessionId,
                                                      std::shared_ptr<SessionRuntimeInfo>,
                                                      LogicalSessionIdHash>;

    /**
     * Must be called with _mutex locked and returns it locked. May release and re-acquire it zero
     * or more times before returning. The returned 'SessionRuntimeInfo' is guaranteed to be linked
     * on the catalog's _txnTable as long as the lock is held.
     */
    std::shared_ptr<SessionRuntimeInfo> _getOrCreateSessionRuntimeInfo(
        OperationContext* opCtx, const LogicalSessionId& lsid, stdx::unique_lock<stdx::mutex>& ul);

    /**
     * Makes a session, previously checked out through 'checkoutSession', available again.
     */
    void _releaseSession(const LogicalSessionId& lsid);

    ServiceContext* const _serviceContext;

    stdx::mutex _mutex;
    SessionRuntimeInfoMap _txnTable;
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
        return &_sri->txnState;
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

public:
    ScopedCheckedOutSession(ScopedCheckedOutSession&&) = default;

    ~ScopedCheckedOutSession() {
        if (_scopedSession) {
            SessionCatalog::get(_opCtx)->_releaseSession(_scopedSession->getSessionId());
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
    ScopedCheckedOutSession(OperationContext* opCtx, ScopedSession scopedSession)
        : _opCtx(opCtx), _scopedSession(std::move(scopedSession)) {}

    OperationContext* const _opCtx;

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
    OperationContextSession(OperationContext* opCtx, bool checkOutSession);
    ~OperationContextSession();

    static Session* get(OperationContext* opCtx);

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
