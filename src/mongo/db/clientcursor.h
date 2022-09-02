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
#include <functional>

#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/session/logical_session_id.h"

namespace mongo {

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
    ClientCursorParams(std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor,
                       NamespaceString nss,
                       boost::optional<UserName> authenticatedUser,
                       APIParameters apiParameters,
                       WriteConcernOptions writeConcernOptions,
                       repl::ReadConcernArgs readConcernArgs,
                       ReadPreferenceSetting readPreferenceSetting,
                       BSONObj originatingCommandObj,
                       PrivilegeVector originatingPrivileges)
        : exec(std::move(planExecutor)),
          nss(std::move(nss)),
          authenticatedUser(std::move(authenticatedUser)),
          apiParameters(std::move(apiParameters)),
          writeConcernOptions(std::move(writeConcernOptions)),
          readConcernArgs(std::move(readConcernArgs)),
          readPreferenceSetting(std::move(readPreferenceSetting)),
          isNoTimeout(
              exec->getCanonicalQuery()
                  ? static_cast<bool>(
                        exec->getCanonicalQuery()->getFindCommandRequest().getNoCursorTimeout())
                  : false),
          tailableMode(exec->getCanonicalQuery()
                           ? query_request_helper::getTailableMode(
                                 exec->getCanonicalQuery()->getFindCommandRequest())
                           : TailableModeEnum::kNormal),
          originatingCommandObj(originatingCommandObj.getOwned()),
          originatingPrivileges(std::move(originatingPrivileges)) {}

    void setTailableMode(TailableModeEnum newMode) {
        tailableMode = newMode;
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
    const NamespaceString nss;
    boost::optional<UserName> authenticatedUser;
    const APIParameters apiParameters;
    const WriteConcernOptions writeConcernOptions;
    const repl::ReadConcernArgs readConcernArgs;
    const ReadPreferenceSetting readPreferenceSetting;
    const bool isNoTimeout;
    TailableModeEnum tailableMode;
    BSONObj originatingCommandObj;
    PrivilegeVector originatingPrivileges;
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
    ClientCursor(const ClientCursor&) = delete;
    ClientCursor& operator=(const ClientCursor&) = delete;

public:
    CursorId cursorid() const {
        return _cursorid;
    }

    const NamespaceString& nss() const {
        return _nss;
    }

    boost::optional<UserName> getAuthenticatedUser() const {
        return _authenticatedUser;
    }

    boost::optional<LogicalSessionId> getSessionId() const {
        return _lsid;
    }

    boost::optional<TxnNumber> getTxnNumber() const {
        return _txnNumber;
    }

    APIParameters getAPIParameters() const {
        return _apiParameters;
    }

    WriteConcernOptions getWriteConcernOptions() const {
        return _writeConcernOptions;
    }

    repl::ReadConcernArgs getReadConcernArgs() const {
        return _readConcernArgs;
    }

    ReadPreferenceSetting getReadPreferenceSetting() const {
        return _readPreferenceSetting;
    }

    /**
     * Returns a pointer to the underlying query plan executor. All cursors manage a PlanExecutor,
     * so this method never returns a null pointer.
     */
    PlanExecutor* getExecutor() const {
        return _exec.get();
    }

    bool isTailable() const {
        switch (_tailableMode) {
            case TailableModeEnum::kNormal:
                return false;
            case TailableModeEnum::kTailable:
            case TailableModeEnum::kTailableAndAwaitData:
                return true;
        }

        MONGO_UNREACHABLE;
    }

    bool isAwaitData() const {
        return _tailableMode == TailableModeEnum::kTailableAndAwaitData;
    }

    /**
     * Returns the original command object which created this cursor.
     */
    const BSONObj& getOriginatingCommandObj() const {
        return _originatingCommand;
    }

    /**
     * Returns the privileges required to run a getMore against this cursor. This is the same as the
     * set of privileges which would have been required to create the cursor in the first place.
     */
    const PrivilegeVector& getOriginatingPrivileges() const& {
        return _originatingPrivileges;
    }
    void getOriginatingPrivileges() && = delete;

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

    /**
     * Returns a generic cursor containing diagnostics about this cursor.
     * The caller must either have this cursor pinned or hold a mutex from the cursor manager.
     */
    GenericCursor toGenericCursor() const;

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
     * Returns the commit point at the time the last batch was returned.
     */
    boost::optional<repl::OpTime> getLastKnownCommittedOpTime() const {
        return _lastKnownCommittedOpTime;
    }

    /**
     * Sets the commit point at the time the last batch was returned.
     */
    void setLastKnownCommittedOpTime(boost::optional<repl::OpTime> lastCommittedOpTime) {
        _lastKnownCommittedOpTime = std::move(lastCommittedOpTime);
    }

    friend std::size_t partitionOf(const ClientCursor* cursor) {
        return cursor->cursorid();
    }

    boost::optional<OperationKey> getOperationKey() const {
        return _opKey;
    }

    std::unique_ptr<RecoveryUnit> releaseStashedRecoveryUnit() {
        return std::move(_stashedRecoveryUnit);
    }

    void stashRecoveryUnit(std::unique_ptr<RecoveryUnit> ru) {
        _stashedRecoveryUnit = std::move(ru);
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
        return _isNoTimeout;
    }

    // The ID of the ClientCursor. A value of 0 is used to mean that no cursor id has been assigned.
    const CursorId _cursorid = 0;

    // Threads may read from this field even if they don't have the cursor pinned, as long as they
    // have the correct partition of the CursorManager locked (just like _authenticatedUsers).
    const NamespaceString _nss;

    // The authenticated user when this cursor was created. Threads may read from this
    // field (using the getter) even if they don't have the cursor pinned as long as they hold the
    // correct partition's lock in the CursorManager. They must hold the lock to prevent the cursor
    // from being freed by another thread during the read.
    const boost::optional<UserName> _authenticatedUser;

    // A logical session id for this cursor, if it is running inside of a session.
    const boost::optional<LogicalSessionId> _lsid;

    // A transaction number for this cursor, if it was provided in the originating command.
    const boost::optional<TxnNumber> _txnNumber;

    const APIParameters _apiParameters;
    const WriteConcernOptions _writeConcernOptions;
    const repl::ReadConcernArgs _readConcernArgs;
    const ReadPreferenceSetting _readPreferenceSetting;

    // Tracks whether dispose() has been called, to make sure it happens before destruction. It is
    // an error to use a ClientCursor once it has been disposed.
    bool _disposed = false;

    // Tracks the number of results returned by this cursor so far. Tracked only as debugging info
    // for display in $currentOp output.
    std::uint64_t _nReturnedSoFar = 0;

    // Tracks the number of batches returned by this cursor so far.
    std::uint64_t _nBatchesReturned = 0;

    // Holds an owned copy of the command specification received from the client.
    const BSONObj _originatingCommand;

    // The privileges required for the _originatingCommand.
    const PrivilegeVector _originatingPrivileges;

    const TailableModeEnum _tailableMode;

    const bool _isNoTimeout;

    // Unused maxTime budget for this cursor.
    Microseconds _leftoverMaxTimeMicros = Microseconds::max();

    // Stashed recovery unit. Maintains valid and positioned cursors across commands, so that data
    // pointers remain valid and safe to access. May be nullptr. This field MUST come before
    // '_exec' as we cannot destroy the recovery unit until the plan executor and its resources
    // (cursors) have been destroyed.
    std::unique_ptr<RecoveryUnit> _stashedRecoveryUnit;

    // The underlying query execution machinery. Must be non-null.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;

    // While a cursor is being used by a client, it is marked as "pinned" by setting
    // _operationUsingCursor to the current OperationContext.
    //
    // Cursors always come into existence in a pinned state ('_operationUsingCursor' must be
    // non-null at construction).
    //
    // To write to this field one of the following must be true:
    // 1) You have a lock on the appropriate partition in CursorManager and the cursor is unpinned
    // (the field is null).
    // 2) The cursor has already been deregistered from the CursorManager. In this case, nobody else
    // will try to pin the cursor.
    //
    // To read this field one of the following must be true:
    // 1) You have a lock on the appropriate partition in CursorManager.
    // 2) You know you have the cursor pinned.
    OperationContext* _operationUsingCursor;

    Date_t _lastUseDate;
    Date_t _createdDate;

    // A string with the plan summary of the cursor's query.
    std::string _planSummary;

    // Commit point at the time the last batch was returned. This is only used by internal exhaust
    // oplog fetching.
    boost::optional<repl::OpTime> _lastKnownCommittedOpTime;

    // Passed along from the original query so that it can be logged if necessary in getMore
    // requests.
    boost::optional<uint32_t> _planCacheKey;
    boost::optional<uint32_t> _queryHash;

    // The client OperationKey associated with this cursor.
    boost::optional<OperationKey> _opKey;
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
 * requests (though they can be marked as interrupted).
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
 *
 *     // Use the PlanExecutor associated with the cursor.
 *     auto next = cursor->planExecutor()->getNext();
 *
 * }
 *
 * Callers need not hold any lock manager locks in order to obtain or release a client cursor pin.
 * However, in order to use the ClientCursor itself, locks may need to be acquired. Whether locks
 * are needed to use the ClientCursor can be determined by consulting the ClientCursor's lock
 * policy.
 */
class ClientCursorPin {
    ClientCursorPin(const ClientCursorPin&) = delete;
    ClientCursorPin& operator=(const ClientCursorPin&) = delete;

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
     * Releases the pin without deleting the underlying cursor. Turns into a no-op if release() or
     * deleteUnderlying() have already been called on this pin.
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

    /*
     * Unstashes resources in the cursor onto the operation context using the cursor. This _must_
     * be called before using the plan executor associated with the cursor.
     */
    void unstashResourcesOntoOperationContext();

    /**
     * Inverse of above: Transfers resources which need the same lifetime as the cursor from the
     * operation context to the cursor itself.
     */
    void stashResourcesFromOperationContext();

private:
    friend class CursorManager;

    ClientCursorPin(OperationContext* opCtx, ClientCursor* cursor, CursorManager* cursorManager);

    OperationContext* _opCtx = nullptr;
    ClientCursor* _cursor = nullptr;
    CursorManager* _cursorManager = nullptr;

    // A pinned cursor takes ownership of storage resources (storage-level cursors owned by the
    // PlanExecutor) without lock-manager locks. Such an operation must ensure interruptibility when
    // later acquiring a lock in order to avoid deadlocking with replication rollback at the storage
    // engine level. Rollback signals interrupt to active readers, acquires a global X lock and then
    // waits for all storage cursors to be closed in order to proceed; while a pinned cursor
    // operation holds storage-level cursors and then may try to acquire a lock.
    //
    // An operation holding a pinned cursor must never have an UninterruptibleLockGuard on the stack
    // that causes lock acquisition to hang without checking for interrupt. This
    // InterruptibleLockGuard ensures that operations holding a ClientCursorPin will eventually
    // observe and obey interrupt signals in the locking layer.
    std::unique_ptr<InterruptibleLockGuard> _interruptibleLockGuard;

    bool _shouldSaveRecoveryUnit = false;
};

void startClientCursorMonitor();

}  // namespace mongo
