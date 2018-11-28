
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

#include "mongo/db/auth/user_name.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class Collection;
class CursorManager;
class RecoveryUnit;

/**
 * Parameters used for constructing a ClientCursor. Makes an owned copy of 'originatingCommandObj'
 * to be used across getMores.
 *
 * ClientCursors cannot be constructed in isolation, but rather must be constructed and managed
 * using a CursorManager. See cursor_manager.h for more details.
 */
struct ClientCursorParams {
    // Describes whether callers should acquire locks when using a ClientCursor. Not all cursors
    // have the same locking behavior. In particular, find cursors require the caller to lock the
    // collection in MODE_IS before calling methods on the underlying plan executor. Aggregate
    // cursors, on the other hand, may access multiple collections and acquire their own locks on
    // any involved collections while producing query results. Therefore, the caller need not
    // explicitly acquire any locks when using a ClientCursor which houses execution machinery for
    // an aggregate.
    //
    // The policy is consulted on getMore in order to determine locking behavior, since during
    // getMore we otherwise could not easily know what flavor of cursor we're using.
    enum class LockPolicy {
        // The caller is responsible for locking the collection over which this ClientCursor
        // executes.
        kLockExternally,

        // The caller need not hold no locks; this ClientCursor's plan executor acquires any
        // necessary locks itself.
        kLocksInternally,
    };

    ClientCursorParams(std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor,
                       NamespaceString nss,
                       UserNameIterator authenticatedUsersIter,
                       repl::ReadConcernArgs readConcernArgs,
                       BSONObj originatingCommandObj,
                       LockPolicy lockPolicy)
        : exec(std::move(planExecutor)),
          nss(std::move(nss)),
          readConcernArgs(readConcernArgs),
          queryOptions(exec->getCanonicalQuery()
                           ? exec->getCanonicalQuery()->getQueryRequest().getOptions()
                           : 0),
          originatingCommandObj(originatingCommandObj.getOwned()),
          lockPolicy(lockPolicy) {
        while (authenticatedUsersIter.more()) {
            authenticatedUsers.emplace_back(authenticatedUsersIter.next());
        }
    }

    void setTailable(bool tailable) {
        if (tailable)
            queryOptions |= QueryOption_CursorTailable;
        else
            queryOptions &= ~QueryOption_CursorTailable;
    }

    void setAwaitData(bool awaitData) {
        if (awaitData)
            queryOptions |= QueryOption_AwaitData;
        else
            queryOptions &= ~QueryOption_AwaitData;
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
    const NamespaceString nss;
    std::vector<UserName> authenticatedUsers;
    const repl::ReadConcernArgs readConcernArgs;
    int queryOptions = 0;
    BSONObj originatingCommandObj;
    const LockPolicy lockPolicy;
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

    boost::optional<LogicalSessionId> getSessionId() const {
        return _lsid;
    }

    boost::optional<TxnNumber> getTxnNumber() const {
        return _txnNumber;
    }

    repl::ReadConcernArgs getReadConcernArgs() const {
        return _readConcernArgs;
    }

    /**
     * Returns a pointer to the underlying query plan executor. All cursors manage a PlanExecutor,
     * so this method never returns a null pointer.
     */
    PlanExecutor* getExecutor() const {
        return _exec.get();
    }

    /**
     * Returns the query options bitmask.  If you'd like to know if the cursor is tailable or
     * awaitData, prefer using the specific methods isTailable() and isAwaitData() over using this
     * method.
     */
    int queryOptions() const {
        return _queryOptions;
    }

    bool isTailable() const {
        return _queryOptions & QueryOption_CursorTailable;
    }

    bool isAwaitData() const {
        return _queryOptions & QueryOption_AwaitData;
    }

    const BSONObj& getOriginatingCommandObj() const {
        return _originatingCommand;
    }

    /**
     * Returns the total number of query results returned by the cursor so far.
     */
    std::uint64_t nReturnedSoFar() const {
        return _nReturnedSoFar;
    }

    /**
     * Increments the cursor's tracked number of query results returned so far by 'n'.
     */
    void incNReturnedSoFar(std::uint64_t n) {
        _nReturnedSoFar += n;
    }

    /**
     * Sets the cursor's tracked number of query results returned so far to 'n'.
     */
    void setNReturnedSoFar(std::uint64_t n) {
        invariant(n >= _nReturnedSoFar);
        _nReturnedSoFar = n;
    }

    /**
     * Returns the number of batches returned by this cursor so far.
     */
    std::uint64_t getNBatches() const {
        return _nBatchesReturned;
    }

    /**
     * Increments the number of batches returned so far by one.
     */
    void incNBatches() {
        ++_nBatchesReturned;
    }

    Date_t getLastUseDate() const {
        return _lastUseDate;
    }

    Date_t getCreatedDate() const {
        return _createdDate;
    }

    void setPlanSummary(std::string ps) {
        _planSummary = std::move(ps);
    }

    StringData getPlanSummary() const {
        return StringData(_planSummary);
    }

    ClientCursorParams::LockPolicy lockPolicy() const {
        return _lockPolicy;
    }

    /**
     * Returns a generic cursor containing diagnostics about this cursor.
     * The caller must either have this cursor pinned or hold a mutex from the cursor manager.
     */
    GenericCursor toGenericCursor() const;

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

    /**
     * Returns the server-wide the count of living cursors. Such a cursor is called an "open
     * cursor".
     */
    static long long totalOpen();

    friend std::size_t partitionOf(const ClientCursor* cursor) {
        return cursor->cursorid();
    }

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
    ClientCursor(ClientCursorParams params,
                 CursorManager* cursorManager,
                 CursorId cursorId,
                 OperationContext* operationUsingCursor,
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
     * Marks this cursor as killed, so any future uses will return 'killStatus'. It is an error to
     * call this method with Status::OK.
     */
    void markAsKilled(Status killStatus);

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
    const CursorId _cursorid = 0;

    // Threads may read from this field even if they don't have the cursor pinned, as long as they
    // have the correct partition of the CursorManager locked (just like _authenticatedUsers).
    const NamespaceString _nss;

    // The set of authenticated users when this cursor was created. Threads may read from this
    // field (using the getter) even if they don't have the cursor pinned as long as they hold the
    // correct partition's lock in the CursorManager. They must hold the lock to prevent the cursor
    // from being freed by another thread during the read.
    const std::vector<UserName> _authenticatedUsers;

    // A logical session id for this cursor, if it is running inside of a session.
    const boost::optional<LogicalSessionId> _lsid;

    // A transaction number for this cursor, if it was provided in the originating command.
    const boost::optional<TxnNumber> _txnNumber;

    const repl::ReadConcernArgs _readConcernArgs;

    CursorManager* _cursorManager;

    // Tracks whether dispose() has been called, to make sure it happens before destruction. It is
    // an error to use a ClientCursor once it has been disposed.
    bool _disposed = false;

    // Tracks the number of results returned by this cursor so far.
    std::uint64_t _nReturnedSoFar = 0;

    // Tracks the number of batches returned by this cursor so far.
    std::uint64_t _nBatchesReturned = 0;

    // Holds an owned copy of the command specification received from the client.
    const BSONObj _originatingCommand;

    // See the QueryOptions enum in dbclientinterface.h.
    const int _queryOptions = 0;

    const ClientCursorParams::LockPolicy _lockPolicy;

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

    // While a cursor is being used by a client, it is marked as "pinned" by setting
    // _operationUsingCursor to the current OperationContext.
    //
    // Cursors always come into existence in a pinned state (this must be non-null at construction).
    //
    // To write to this field one of the following must be true:
    // 1) You have a lock on the appropriate partition in CursorManager and the cursor is unpinned
    // (the field is null).
    // 2) You own the cursor and the cursor manager it was associated with is gone (this can only
    // happen in ClientCursorPin). In this case, nobody else will try to pin the cursor.
    //
    // To read this field one of the following must be true:
    // 1) You have a lock on the appropriate partition in CursorManager.
    // 2) You know you have the cursor pinned.
    OperationContext* _operationUsingCursor;

    Date_t _lastUseDate;
    Date_t _createdDate;

    // A string with the plan summary of the cursor's query.
    std::string _planSummary;
};

/**
 * ClientCursorPin is an RAII class which must be used in order to access a cursor. On
 * construction, the ClientCursorPin marks its cursor as in use, which is called "pinning" the
 * cursor. On destruction, the ClientCursorPin marks its cursor as no longer in use, which is
 * called "unpinning" the cursor. Pinning is used to prevent multiple concurrent uses of the same
 * cursor--- pinned cursors cannot be deleted or timed out and cannot be used concurrently by other
 * operations such as getMore. They can however, be marked as interrupted and instructed to destroy
 * themselves through killCursors.
 *
 * A pin is obtained using the CursorManager. See cursor_manager.h for more details.
 *
 * A pin extends the lifetime of a ClientCursor object until the pin's release. Pinned ClientCursor
 * objects cannot not be killed due to inactivity, and cannot be immediately erased by user kill
 * requests (though they can be marked as interrupted). When a CursorManager is destroyed (e.g. by
 * a collection drop), ownership of any still-pinned ClientCursor objects is transferred to their
 * managing ClientCursorPin objects.
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

    ClientCursor* operator->() {
        return _cursor;
    }

private:
    friend class CursorManager;

    ClientCursorPin(OperationContext* opCtx, ClientCursor* cursor);

    OperationContext* _opCtx = nullptr;
    ClientCursor* _cursor = nullptr;
};

void startClientCursorMonitor();

}  // namespace mongo
