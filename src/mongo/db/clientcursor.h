/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/message.h"

namespace mongo {

class Collection;
class CursorManager;
class RecoveryUnit;

/**
 * Parameters used for constructing a ClientCursor. Makes an owned copy of 'originatingCommandObj'
 * to be used across getMores.
 *
 * ClientCursors cannot be constructed in isolation, but rather must be
 * constructed and managed using a CursorManager. See cursor_manager.h for more details.
 */
struct ClientCursorParams {
    ClientCursorParams(std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor,
                       NamespaceString nss,
                       UserNameIterator authenticatedUsersIter,
                       bool isReadCommitted,
                       BSONObj originatingCommandObj)
        : exec(std::move(planExecutor)),
          nss(std::move(nss)),
          isReadCommitted(isReadCommitted),
          queryOptions(exec->getCanonicalQuery()
                           ? exec->getCanonicalQuery()->getQueryRequest().getOptions()
                           : 0),
          originatingCommandObj(originatingCommandObj.getOwned()) {
        while (authenticatedUsersIter.more()) {
            authenticatedUsers.emplace_back(authenticatedUsersIter.next());
        }
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
    const NamespaceString nss;
    std::vector<UserName> authenticatedUsers;
    bool isReadCommitted = false;
    int queryOptions = 0;
    BSONObj originatingCommandObj;
};

/**
 * A ClientCursor is the server-side state associated with a particular cursor id. A cursor id is a
 * handle that we return to the client for queries which require results to be returned in multiple
 * batches. The client can manage the server-side cursor state by passing the cursor id back to the
 * server for certain supported operations.
 *
 * For instance, a client can retrieve the next batch of results from the cursor by issuing a
 * getMore on this cursor id. It can also request that server-side resources be freed by issuing a
 * killCursors on a particular cursor id. This is useful if the client wishes to abandon the cursor
 * without retrieving all results.
 *
 * ClientCursors cannot exist in isolation and must be created, accessed, and destroyed via a
 * CursorManager. See cursor_manager.h for more details. Unless the ClientCursor is marked by the
 * caller as "no timeout", it will be automatically destroyed by its cursor manager after a period
 * of inactivity.
 */
class ClientCursor {
    MONGO_DISALLOW_COPYING(ClientCursor);

public:
    CursorId cursorid() const {
        return _cursorid;
    }

    const NamespaceString& nss() const {
        return _nss;
    }

    UserNameIterator getAuthenticatedUsers() const {
        return makeUserNameIterator(_authenticatedUsers.begin(), _authenticatedUsers.end());
    }

    bool isReadCommitted() const {
        return _isReadCommitted;
    }

    /**
     * Returns a pointer to the underlying query plan executor. All cursors manage a PlanExecutor,
     * so this method never returns a null pointer.
     */
    PlanExecutor* getExecutor() const {
        return _exec.get();
    }

    int queryOptions() const {
        return _queryOptions;
    }

    const BSONObj& getOriginatingCommandObj() const {
        return _originatingCommand;
    }

    /**
     * Returns the total number of query results returned by the cursor so far.
     */
    long long pos() const {
        return _pos;
    }

    /**
     * Increments the cursor's tracked number of query results returned so far by 'n'.
     */
    void incPos(long long n) {
        _pos += n;
    }

    /**
     * Sets the cursor's tracked number of query results returned so far to 'n'.
     */
    void setPos(long long n) {
        _pos = n;
    }

    //
    // Timing.
    //

    /**
     * Returns the amount of time execution time available to this cursor. Only valid at the
     * beginning of a getMore request, and only really for use by the maxTime tracking code.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    Microseconds getLeftoverMaxTimeMicros() const {
        return _leftoverMaxTimeMicros;
    }

    /**
     * Sets the amount of execution time available to this cursor. This is only called when an
     * operation that uses a cursor is finishing, to update its remaining time.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    void setLeftoverMaxTimeMicros(Microseconds leftoverMaxTimeMicros) {
        _leftoverMaxTimeMicros = leftoverMaxTimeMicros;
    }

    //
    // Replication-related methods.
    //

    // Used to report replication position only in master-slave, so we keep them as TimeStamp rather
    // than OpTime.
    void updateSlaveLocation(OperationContext* opCtx);

    void slaveReadTill(const Timestamp& t) {
        _slaveReadTill = t;
    }

    /** Just for testing. */
    Timestamp getSlaveReadTill() const {
        return _slaveReadTill;
    }

    /**
     * Returns the server-wide the count of living cursors. Such a cursor is called an "open
     * cursor".
     */
    static long long totalOpen();

private:
    friend class CursorManager;
    friend class ClientCursorPin;

    /**
     * Since the client cursor destructor is private, this is needed for using client cursors with
     * smart pointers.
     */
    struct Deleter {
        void operator()(ClientCursor* cursor) {
            delete cursor;
        }
    };

    /**
     * Constructs a ClientCursor. Since cursors must come into being registered and pinned, this is
     * private. See cursor_manager.h for more details.
     */
    ClientCursor(ClientCursorParams&& params,
                 CursorManager* cursorManager,
                 CursorId cursorId,
                 Date_t now);

    /**
     * Destroys a ClientCursor. This is private, since only the CursorManager or the ClientCursorPin
     * is allowed to destroy a cursor.
     *
     * Cursors must be unpinned and deregistered from the CursorManager before they can be
     * destroyed.
     */
    ~ClientCursor();

    /**
     * Marks this cursor as killed, so any future uses will return an error status including
     * 'reason'.
     */
    void markAsKilled(const std::string& reason);

    /**
     * Disposes this ClientCursor's PlanExecutor. Must be called before deleting a ClientCursor to
     * ensure it has a chance to clean up any resources it is using. Can be called multiple times.
     * It is an error to call any other method after calling dispose().
     */
    void dispose(OperationContext* opCtx);

    bool isNoTimeout() const {
        return (_queryOptions & QueryOption_NoCursorTimeout);
    }

    // The ID of the ClientCursor. A value of 0 is used to mean that no cursor id has been assigned.
    CursorId _cursorid = 0;

    const NamespaceString _nss;

    // The set of authenticated users when this cursor was created.
    std::vector<UserName> _authenticatedUsers;

    const bool _isReadCommitted = false;

    CursorManager* _cursorManager;

    // Tracks whether dispose() has been called, to make sure it happens before destruction. It is
    // an error to use a ClientCursor once it has been disposed.
    bool _disposed = false;

    // Tracks the number of results returned by this cursor so far.
    long long _pos = 0;

    // Holds an owned copy of the command specification received from the client.
    const BSONObj _originatingCommand;

    // See the QueryOptions enum in dbclientinterface.h.
    const int _queryOptions = 0;

    // The replication position only used in master-slave.
    Timestamp _slaveReadTill;

    // Unused maxTime budget for this cursor.
    Microseconds _leftoverMaxTimeMicros = Microseconds::max();

    // The underlying query execution machinery. Must be non-null.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;

    //
    // The following fields are used by the CursorManager and the ClientCursorPin. In most
    // conditions, they can only be used while holding the CursorManager's mutex. Exceptions
    // include:
    //   - If the ClientCursor is pinned, the CursorManager will never change '_isPinned' until
    //     asked to by the ClientCursorPin.
    //   - It is safe to read '_killed' while holding a collection lock, which must be held when
    //     interacting with a ClientCursorPin.
    //   - A ClientCursorPin can access these members after deregistering the cursor from the
    //     CursorManager, at which point it has sole ownership of the ClientCursor.
    //

    // TODO SERVER-28309 Remove this field and instead use _exec->markedAsKilled().
    bool _killed = false;

    // While a cursor is being used by a client, it is marked as "pinned". See ClientCursorPin
    // below.
    //
    // Cursors always come into existence in a pinned state.
    bool _isPinned = true;

    Date_t _lastUseDate;
};

/**
 * ClientCursorPin is an RAII class which must be used in order to access a cursor. On construction,
 * the ClientCursorPin marks its cursor as in use, which is called "pinning" the cursor. On
 * destructrution, the ClientCursorPin marks its cursor as no longer in use, which is called
 * "unpinning" the cursor. Pinning is used to prevent multiple concurrent uses of the same cursor---
 * pinned cursors cannot be killed or timed out and cannot be used concurrently by other operations
 * such as getMore or killCursors. A pin is obtained using the CursorManager. See cursor_manager.h
 * for more details.
 *
 * A pin extends the lifetime of a ClientCursor object until the pin's release.  Pinned
 * ClientCursor objects cannot not be killed due to inactivity, and cannot be killed by user
 * kill requests.  When a CursorManager is destroyed (e.g. by a collection drop), ownership of
 * any still-pinned ClientCursor objects is transferred to their managing ClientCursorPin
 * objects.
 *
 * Example usage:
 * {
 *     StatusWith<ClientCursorPin> pin = cursorManager->pinCursor(opCtx, cursorid);
 *     if (!pin.isOK()) {
 *         // No cursor with id 'cursorid' exists, or it was killed while inactive. Handle the error
 *         here.
 *         return pin.getStatus();
 *     }
 *
 *     ClientCursor* cursor = pin.getValue().getCursor();
 *     // Use cursor. Pin automatically released on block exit.
 * }
 *
 * Clients that wish to access ClientCursor objects owned by collection cursor managers must hold
 * the collection lock while calling any pin method, including pin acquisition by the RAII
 * constructor and pin release by the RAII destructor.  This guards from a collection drop (which
 * requires an exclusive lock on the collection) occurring concurrently with the pin request or
 * unpin request.
 *
 * Clients that wish to access ClientCursor objects owned by the global cursor manager need not
 * hold any locks; the global cursor manager can only be destroyed by a process exit.
 */
class ClientCursorPin {
    MONGO_DISALLOW_COPYING(ClientCursorPin);

public:
    /**
     * Moves 'other' into 'this'. The 'other' pin must have a pinned cursor. Moving an empty pin
     * into 'this' is illegal.
     */
    ClientCursorPin(ClientCursorPin&& other);

    /**
     * Moves 'other' into 'this'. 'other' must have a pinned cursor and 'this' must have no pinned
     * cursor.
     */
    ClientCursorPin& operator=(ClientCursorPin&& other);

    /**
     * Calls release().
     */
    ~ClientCursorPin();

    /**
     * Releases the pin.  It does not delete the underlying cursor unless ownership has passed
     * to us after kill.  Turns into a no-op if release() or deleteUnderlying() have already
     * been called on this pin.
     */
    void release();

    /**
     * Deletes the underlying cursor.  Cannot be called if release() or deleteUnderlying() have
     * already been called on this pin.
     */
    void deleteUnderlying();

    /**
     * Returns a pointer to the pinned cursor.
     */
    ClientCursor* getCursor() const;

private:
    friend class CursorManager;

    ClientCursorPin(OperationContext* opCtx, ClientCursor* cursor);

    OperationContext* _opCtx = nullptr;
    ClientCursor* _cursor = nullptr;
};

void startClientCursorMonitor();

}  // namespace mongo
