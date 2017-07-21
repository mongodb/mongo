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
class ServiceContext;

/**
 * Keeps track of the transaction runtime state for every active session on this instance.
 */
class SessionCatalog {
    MONGO_DISALLOW_COPYING(SessionCatalog);

    friend class ScopedSession;

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
    ScopedSession checkOutSession(OperationContext* opCtx);

    /**
     * Clears the entire transaction table. Invoked after rollback.
     */
    void clearTransactionTable();

private:
    struct SessionRuntimeInfo {
        SessionRuntimeInfo(LogicalSessionId lsid) : txnState(std::move(lsid)) {}

        enum State {
            // Session can be checked out
            kAvailable,

            // Session is in use by another operation and the caller must wait to check it out
            kInUse,

            // Session is at the end of its lifetime and is in a state where its persistent
            // information is being cleaned up. Sessions in this state can never be checked out. The
            // reason to put the session in this state is to allow for cleanup to happen
            // asynchronous and while not holding locks.
            kInCleanup
        };

        // Current state of the runtime info for a session
        State state{kAvailable};

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
     * Returns a session, previously clecked out through 'checkoutSession', available again.
     */
    void _releaseSession(const LogicalSessionId& lsid);

    ServiceContext* const _serviceContext;

    stdx::mutex _mutex;
    SessionRuntimeInfoMap _txnTable;
};

/**
 * Scoped object representing a checked-out session. See comments for the 'checkoutSession' method
 * for more information on its behaviour.
 */
class ScopedSession {
    MONGO_DISALLOW_COPYING(ScopedSession);

public:
    ScopedSession(OperationContext* opCtx, std::shared_ptr<SessionCatalog::SessionRuntimeInfo> sri)
        : _opCtx(opCtx), _sri(std::move(sri)) {}

    ScopedSession(ScopedSession&&) = default;

    ~ScopedSession() {
        if (_sri) {
            SessionCatalog::get(_opCtx)->_releaseSession(_sri->txnState.getSessionId());
        }
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

private:
    OperationContext* const _opCtx;

    std::shared_ptr<SessionCatalog::SessionRuntimeInfo> _sri;
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

    static Session* get(OperationContext* opCtx);

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
