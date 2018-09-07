/*
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

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class UpdateRequest;

/**
 * A write through cache for the state of a particular session. All modifications to the underlying
 * session transactions collection must be performed through an object of this class.
 *
 * The cache state can be 'up-to-date' (it is in sync with the persistent contents) or 'needs
 * refresh' (in which case refreshFromStorageIfNeeded needs to be called in order to make it
 * up-to-date).
 */
class Session : public Decorable<Session> {
    MONGO_DISALLOW_COPYING(Session);

public:
    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;

    static const BSONObj kDeadEndSentinel;

    explicit Session(LogicalSessionId sessionId);

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    struct RefreshState {
        long long refreshCount{0};
        TxnNumber txnNumber{kUninitializedTxnNumber};
        bool isCommitted{false};
    };

    /**
     * Blocking method, which loads the transaction state from storage if it has been marked as
     * needing refresh.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock.
     */
    void refreshFromStorageIfNeeded(OperationContext* opCtx);

    /**
     * Starts a new transaction on the session, or continues an already active transaction. In this
     * context, a "transaction" is a sequence of operations associated with a transaction number.
     *
     * Throws an exception if:
     *      - An attempt is made to start a transaction with number less than the latest
     *        transaction this session has seen.
     *      - The session has been invalidated.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock. This method must also be called after refreshFromStorageIfNeeded has been called.
     */
    void beginOrContinueTxn(OperationContext* opCtx, TxnNumber txnNumber);

    /**
     * Called after a write under the specified transaction completes while the node is a primary
     * and specifies the statement ids which were written. Must be called while the caller is still
     * in the write's WUOW. Updates the on-disk state of the session to match the specified
     * transaction/opTime and keeps the cached state in sync.
     *
     * 'txnState' is 'none' for retryable writes.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    void onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIdsWritten,
                                     const repl::OpTime& lastStmtIdWriteOpTime,
                                     Date_t lastStmtIdWriteDate,
                                     boost::optional<DurableTxnStateEnum> txnState);

    /**
     * Helper function to begin a migration on a primary node.
     *
     * Returns whether the specified statement should be migrated at all or skipped.
     *
     * Not called with session checked out.
     */
    bool onMigrateBeginOnPrimary(OperationContext* opCtx, TxnNumber txnNumber, StmtId stmtId);

    /**
     * Called after an entry for the specified session and transaction has been written to the oplog
     * during chunk migration, while the node is still primary. Must be called while the caller is
     * still in the oplog write's WUOW. Updates the on-disk state of the session to match the
     * specified transaction/opTime and keeps the cached state in sync.
     *
     * May be called concurrently with onWriteOpCompletedOnPrimary or onMigrateCompletedOnPrimary
     * and doesn't require the session to be checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number is newer than the
     * one specified.
     */
    void onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIdsWritten,
                                     const repl::OpTime& lastStmtIdWriteOpTime,
                                     Date_t oplogLastStmtIdWriteDate);

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    void invalidate();

    /**
     * Returns the op time of the last committed write for this session and transaction. If no write
     * has completed yet, returns an empty timestamp.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    repl::OpTime getLastWriteOpTime(TxnNumber txnNumber) const;

    /**
     * Checks whether the given statementId for the specified transaction has already executed and
     * if so, returns the oplog entry which was generated by that write. If the statementId hasn't
     * executed, returns boost::none.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    boost::optional<repl::OplogEntry> checkStatementExecuted(OperationContext* opCtx,
                                                             TxnNumber txnNumber,
                                                             StmtId stmtId) const;

    /**
     * Checks whether the given statementId for the specified transaction has already executed
     * without fetching the oplog entry which was generated by that write.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    bool checkStatementExecutedNoOplogEntryFetch(TxnNumber txnNumber, StmtId stmtId) const;

    TxnNumber getActiveTxnNumber() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _activeTxnNumber;
    }

    /**
     * Returns a new oplog entry if the given entry has transaction state embedded within in.
     * The new oplog entry will contain the operation needed to replicate the transaction
     * table.
     * Returns boost::none if the given oplog doesn't have any transaction state or does not
     * support update to the transaction table.
     */
    static boost::optional<repl::OplogEntry> createMatchingTransactionTableUpdate(
        const repl::OplogEntry& entry);

    /**
     * Returns the state of the session from storage the last time a refresh occurred.
     */
    boost::optional<RefreshState> getLastRefreshState() const;

    /**
     * Attempt to lock the active TxnNumber of this session to the given number. This operation
     * can only succeed if it is equal to the current active TxnNumber. Also sets the error status
     * for any callers trying to modify the TxnNumber.
     */
    void lockTxnNumber(const TxnNumber lockThisNumber, Status conflictError);

    /**
     * Release the lock on the active TxnNumber and allow it to be modified.
     */
    void unlockTxnNumber();

    /**
     * Returns if the given TxnNumber is locked.
     */
    bool isLockedTxnNumber(const TxnNumber expectedLockedNumber) const;

    /**
     * Sets the current operation running on this Session.
     */
    void setCurrentOperation(OperationContext* currentOperation);

    /**
     * Clears the current operation running on this Session.
     */
    void clearCurrentOperation();

    /**
     * Returns a pointer to the current operation running on this Session, or nullptr if there is no
     * operation currently running on this Session.
     */
    OperationContext* getCurrentOperation() const;

private:
    void _beginOrContinueTxn(WithLock, TxnNumber txnNumber);

    // Checks if there is a conflicting operation on the current Session
    void _checkValid(WithLock) const;

    // Checks that a new txnNumber is higher than the activeTxnNumber so
    // we don't start a txn that is too old.
    void _checkTxnValid(WithLock, TxnNumber txnNumber) const;

    void _setActiveTxn(WithLock, TxnNumber txnNumber);

    void _checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const;

    boost::optional<repl::OpTime> _checkStatementExecuted(WithLock,
                                                          TxnNumber txnNumber,
                                                          StmtId stmtId) const;

    // Returns the write date of the last committed write for this session and transaction. If no
    // write has completed yet, returns an empty date.
    //
    // Throws if the session has been invalidated or the active transaction number doesn't match.
    Date_t _getLastWriteDate(WithLock, TxnNumber txnNumber) const;

    UpdateRequest _makeUpdateRequest(WithLock,
                                     TxnNumber newTxnNumber,
                                     const repl::OpTime& newLastWriteTs,
                                     Date_t newLastWriteDate,
                                     boost::optional<DurableTxnStateEnum> newState) const;

    void _registerUpdateCacheOnCommit(OperationContext* opCtx,
                                      TxnNumber newTxnNumber,
                                      std::vector<StmtId> stmtIdsWritten,
                                      const repl::OpTime& lastStmtIdWriteTs);

    const LogicalSessionId _sessionId;

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // A pointer back to the currently running operation on this Session, or nullptr if there
    // is no operation currently running for the Session.
    OperationContext* _currentOperation{nullptr};

    // Specifies whether the session information needs to be refreshed from storage
    bool _isValid{false};

    // Counter, incremented with each call to invalidate in order to discern invalidations, which
    // happen during refresh
    int _numInvalidations{0};

    // Set to true if incomplete history is detected. For example, when the oplog to a write was
    // truncated because it was too old.
    bool _hasIncompleteHistory{false};

    // Caches what is known to be the last written transaction record for the session
    boost::optional<SessionTxnRecord> _lastWrittenSessionRecord;

    // Tracks the last seen txn number for the session and is always >= to the transaction number in
    // the last written txn record. When it is > than that in the last written txn record, this
    // means a new transaction has begun on the session, but it hasn't yet performed any writes.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};

    // For the active txn, tracks which statement ids have been committed and at which oplog
    // opTime. Used for fast retryability check and retrieving the previous write's data without
    // having to scan through the oplog.
    CommittedStatementTimestampMap _activeTxnCommittedStatements;

    // Stores the state from last refresh.
    boost::optional<RefreshState> _lastRefreshState;

    // True if txnNumber cannot be modified.
    bool _isTxnNumberLocked{false};

    // The status to return when an operation tries to modify the active TxnNumber while it is
    // locked.
    boost::optional<Status> _txnNumberLockConflictStatus;
};

}  // namespace mongo
