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
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

extern AtomicInt32 transactionLifetimeLimitSeconds;

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
    /**
     * Holds state for a snapshot read or multi-statement transaction in between network operations.
     */
    class TxnResources {
    public:
        /**
         * Stashes transaction state from 'opCtx' in the newly constructed TxnResources.
         * keepTicket: If true, do not release locker's throttling ticket.
         *             Use only for short-term stashing.
         */
        TxnResources(OperationContext* opCtx, bool keepTicket = false);

        ~TxnResources();

        // Rule of 5: because we have a class-defined destructor, we need to explictly specify
        // the move operator and move assignment operator.
        TxnResources(TxnResources&&) = default;
        TxnResources& operator=(TxnResources&&) = default;

        /**
         * Returns a const pointer to the stashed lock state, or nullptr if no stashed locks exist.
         */
        const Locker* locker() const {
            return _locker.get();
        }

        /**
         * Releases stashed transaction state onto 'opCtx'. Must only be called once.
         */
        void release(OperationContext* opCtx);

        /**
         * Returns the read concern arguments.
         */
        repl::ReadConcernArgs getReadConcernArgs() const {
            return _readConcernArgs;
        }

    private:
        bool _released = false;
        std::unique_ptr<Locker> _locker;
        std::unique_ptr<RecoveryUnit> _recoveryUnit;
        repl::ReadConcernArgs _readConcernArgs;
        WriteUnitOfWork::RecoveryUnitState _ruState;
    };

    /**
     *  An RAII object that stashes `TxnResouces` from the `opCtx` onto the stack. At destruction
     *  it unstashes the `TxnResources` back onto the `opCtx`.
     */
    class SideTransactionBlock {
    public:
        SideTransactionBlock(OperationContext* opCtx);
        ~SideTransactionBlock();

        // Rule of 5: because we have a class-defined destructor, we need to explictly specify
        // the move operator and move assignment operator.
        SideTransactionBlock(SideTransactionBlock&&) = default;
        SideTransactionBlock& operator=(SideTransactionBlock&&) = default;

    private:
        boost::optional<Session::TxnResources> _txnResources;
        OperationContext* _opCtx;
    };

    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;
    using CursorExistsFunction = std::function<bool(LogicalSessionId, TxnNumber)>;

    static const BSONObj kDeadEndSentinel;

    explicit Session(LogicalSessionId sessionId);

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

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
     * This sequence of operations could be a retryable write or multi-statement transaction. Both
     * utilize this method.
     *
     * The 'autocommit' argument represents the value of the field given in the original client
     * request. If it is boost::none, no autocommit parameter was passed into the request. Every
     * operation that is part of a multi statement transaction must specify 'autocommit=false'.
     * 'startTransaction' represents the value of the field given in the original client request,
     * and indicates whether this operation is the beginning of a multi-statement transaction.
     *
     * Throws an exception if:
     *      - An attempt is made to start a transaction with number less than the latest
     *        transaction this session has seen.
     *      - The session has been invalidated.
     *      - The values of 'autocommit' and/or 'startTransaction' are inconsistent with the current
     *        state of the transaction.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock. This method must also be called after refreshFromStorageIfNeeded has been called.
     */
    void beginOrContinueTxn(OperationContext* opCtx,
                            TxnNumber txnNumber,
                            boost::optional<bool> autocommit,
                            boost::optional<bool> startTransaction,
                            StringData dbName,
                            StringData cmdName);
    /**
     * Similar to beginOrContinueTxn except it is used specifically for shard migrations and does
     * not check or modify the autocommit parameter.
     *
     * Not called with session checked out.
     */
    void beginOrContinueTxnOnMigration(OperationContext* opCtx, TxnNumber txnNumber);

    /**
     * Called for speculative transactions to fix the optime of the snapshot to read from.
     */
    void setSpeculativeTransactionOpTimeToLastApplied(OperationContext* opCtx);

    /**
     * Called after a write under the specified transaction completes while the node is a primary
     * and specifies the statement ids which were written. Must be called while the caller is still
     * in the write's WUOW. Updates the on-disk state of the session to match the specified
     * transaction/opTime and keeps the cached state in sync.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    void onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIdsWritten,
                                     const repl::OpTime& lastStmtIdWriteOpTime,
                                     Date_t lastStmtIdWriteDate);

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

    /**
     * Transfers management of transaction resources from the OperationContext to the Session.
     */
    void stashTransactionResources(OperationContext* opCtx);

    /**
     * Transfers management of transaction resources from the Session to the OperationContext.
     */
    void unstashTransactionResources(OperationContext* opCtx, const std::string& cmdName);

    /**
     * Commits the transaction, including committing the write unit of work and updating
     * transaction state.
     *
     * Throws an exception if the transaction is prepared.
     */
    void commitUnpreparedTransaction(OperationContext* opCtx);

    /**
     * Commits the transaction, including committing the write unit of work and updating
     * transaction state.
     *
     * Throws an exception if the transaction is not prepared or if the 'commitTimestamp' is null.
     */
    void commitPreparedTransaction(OperationContext* opCtx, Timestamp commitTimestamp);

    /**
     * Puts a transaction into a prepared state and returns the prepareTimestamp.
     */
    Timestamp prepareTransaction(OperationContext* opCtx);

    /**
     * Aborts the transaction outside the transaction, releasing transaction resources.
     *
     * Not called with session checked out.
     */
    void abortArbitraryTransaction();

    /**
     * Same as abortArbitraryTransaction, except only executes if _transactionExpireDate indicates
     * that the transaction has expired.
     *
     * Not called with session checked out.
     */
    void abortArbitraryTransactionIfExpired();

    /*
     * Aborts the transaction inside the transaction, releasing transaction resources.
     * We're inside the transaction when we have the Session checked out and 'opCtx' owns the
     * transaction resources.
     */
    void abortActiveTransaction(OperationContext* opCtx);

    bool getAutocommit() const {
        return _autocommit;
    }

    /**
     * Returns whether we are in a multi-document transaction, which means we have an active
     * transaction which has autoCommit:false and has not been committed or aborted. It is possible
     * that the current transaction is stashed onto the stack via a `SideTransactionBlock`.
     */
    bool inMultiDocumentTransaction() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _txnState.inMultiDocumentTransaction(lk);
    };

    bool transactionIsCommitted() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _txnState.isCommitted(lk);
    }

    bool transactionIsAborted() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _txnState.isAborted(lk);
    }

    /**
     * Adds a stored operation to the list of stored operations for the current multi-document
     * (non-autocommit) transaction.  It is illegal to add operations when no multi-document
     * transaction is in progress.
     */
    void addTransactionOperation(OperationContext* opCtx, const repl::ReplOperation& operation);

    /**
     * Returns and clears the stored operations for an multi-document (non-autocommit) transaction,
     * and marks the transaction as closed.  It is illegal to attempt to add operations to the
     * transaction after this is called.
     */
    std::vector<repl::ReplOperation> endTransactionAndRetrieveOperations(OperationContext* opCtx);

    const std::vector<repl::ReplOperation>& transactionOperationsForTest() {
        return _transactionOperations;
    }

    TxnNumber getActiveTxnNumberForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _activeTxnNumber;
    }

    boost::optional<SingleTransactionStats> getSingleTransactionStats() const {
        return _singleTransactionStats;
    }

    repl::OpTime getSpeculativeTransactionReadOpTimeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _speculativeTransactionReadOpTime;
    }

    const Locker* getTxnResourceStashLockerForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_txnResourceStash);
        return _txnResourceStash->locker();
    }

    /**
     * If this session is holding stashed locks in _txnResourceStash, reports the current state of
     * the session using the provided builder. Locks the session object's mutex while running.
     */
    void reportStashedState(BSONObjBuilder* builder) const;

    /**
     * If this session is not holding stashed locks in _txnResourceStash (transaction is active),
     * reports the current state of the session using the provided builder. Locks the session
     * object's mutex while running.
     */
    void reportUnstashedState(repl::ReadConcernArgs readConcernArgs, BSONObjBuilder* builder) const;

    /**
     * Convenience method which creates and populates a BSONObj containing the stashed state.
     * Returns an empty BSONObj if this session has no stashed resources.
     */
    BSONObj reportStashedState() const;

    std::string transactionInfoForLogForTest(const SingleThreadedLockStats* lockStats,
                                             bool committed,
                                             repl::ReadConcernArgs readConcernArgs) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        TransactionState::StateFlag terminationCause =
            committed ? TransactionState::kCommitted : TransactionState::kAborted;
        return _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
    }

    void addMultikeyPathInfo(MultikeyPathInfo info) {
        _multikeyPathInfo.push_back(std::move(info));
    }

    const std::vector<MultikeyPathInfo>& getMultikeyPathInfo() const {
        return _multikeyPathInfo;
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

    void transitionToPreparedforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kPrepared);
    }

    void transitionToCommittingforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kCommittingWithoutPrepare);
    }

private:
    // Holds function which determines whether the CursorManager has client cursor references for a
    // given transaction.
    static CursorExistsFunction _cursorExistsFunction;

    void _beginOrContinueTxn(WithLock,
                             TxnNumber txnNumber,
                             boost::optional<bool> autocommit,
                             boost::optional<bool> startTransaction);

    void _beginOrContinueTxnOnMigration(WithLock, TxnNumber txnNumber);

    // Checks if there is a conflicting operation on the current Session
    void _checkValid(WithLock) const;

    // Checks that a new txnNumber is higher than the activeTxnNumber so
    // we don't start a txn that is too old.
    void _checkTxnValid(WithLock, TxnNumber txnNumber) const;

    void _setActiveTxn(WithLock, TxnNumber txnNumber);

    void _checkIsActiveTransaction(WithLock, TxnNumber txnNumber, bool checkAbort) const;

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
                                     Date_t newLastWriteDate) const;

    void _registerUpdateCacheOnCommit(OperationContext* opCtx,
                                      TxnNumber newTxnNumber,
                                      std::vector<StmtId> stmtIdsWritten,
                                      const repl::OpTime& lastStmtIdWriteTs);

    /**
     * Reserves a slot in the oplog with an open storage-transaction while it is alive. Reserves the
     * slot at construction. Aborts the storage-transaction and releases the oplog slot at
     * destruction.
     */
    class OplogSlotReserver {
    public:
        OplogSlotReserver(OperationContext* opCtx);

        ~OplogSlotReserver();

        // Rule of 5: because we have a class-defined destructor, we need to explictly specify
        // the move operator and move assignment operator.
        OplogSlotReserver(OplogSlotReserver&&) = default;
        OplogSlotReserver& operator=(OplogSlotReserver&&) = default;

        /**
         * Returns the oplog slot reserved at construction.
         */
        OplogSlot getReservedOplogSlot() const {
            invariant(!_oplogSlot.opTime.isNull());
            return _oplogSlot;
        }

    private:
        std::unique_ptr<Locker> _locker;
        std::unique_ptr<RecoveryUnit> _recoveryUnit;
        OplogSlot _oplogSlot;
    };

    /**
     * Indicates the state of the current multi-document transaction, if any.  If the transaction is
     * in any state but kInProgress, no more operations can be collected. Once the transaction is in
     * kPrepared, the transaction is not allowed to abort outside of an 'abortTransaction' command.
     * At this point, aborting the transaction must log an 'abortTransaction' oplog entry.
     */
    class TransactionState {
    public:
        enum StateFlag {
            kNone = 1 << 0,
            kInProgress = 1 << 1,
            kPrepared = 1 << 2,
            kCommittingWithoutPrepare = 1 << 3,
            kCommittingWithPrepare = 1 << 4,
            kCommitted = 1 << 5,
            kAborted = 1 << 6
        };

        using StateSet = int;

        bool isInSet(WithLock, StateSet stateSet) const {
            return _state & stateSet;
        }

        /**
         * Transitions the session from the current state to the new state. If transition validation
         * is not relaxed, invariants if the transition is illegal.
         */
        enum class TransitionValidation { kValidateTransition, kRelaxTransitionValidation };
        void transitionTo(
            WithLock,
            StateFlag newState,
            TransitionValidation shouldValidate = TransitionValidation::kValidateTransition);

        bool inMultiDocumentTransaction(WithLock) const {
            return _state == kInProgress || _state == kPrepared;
        }

        bool isNone(WithLock) const {
            return _state == kNone;
        }

        bool isInProgress(WithLock) const {
            return _state == kInProgress;
        }

        bool isPrepared(WithLock) const {
            return _state == kPrepared;
        }

        bool isCommittingWithoutPrepare(WithLock) const {
            return _state == kCommittingWithoutPrepare;
        }

        bool isCommittingWithPrepare(WithLock) const {
            return _state == kCommittingWithPrepare;
        }

        bool isCommitted(WithLock) const {
            return _state == kCommitted;
        }

        bool isAborted(WithLock) const {
            return _state == kAborted;
        }

        std::string toString() const {
            return toString(_state);
        }

        static std::string toString(StateFlag state);

    private:
        static bool _isLegalTransition(StateFlag oldState, StateFlag newState);

        StateFlag _state = kNone;
    };

    friend std::ostream& operator<<(std::ostream& s, TransactionState txnState) {
        return (s << txnState.toString());
    }

    friend StringBuilder& operator<<(StringBuilder& s, TransactionState txnState) {
        return (s << txnState.toString());
    }

    // Abort the transaction if it's in one of the expected states and clean up the transaction
    // states associated with the opCtx.
    void _abortActiveTransaction(OperationContext* opCtx,
                                 TransactionState::StateSet expectedStates);

    void _abortArbitraryTransaction(WithLock);

    // Releases stashed transaction resources to abort the transaction on the session.
    void _abortTransactionOnSession(WithLock);

    // Clean up the transaction resources unstashed on operation context.
    void _cleanUpTxnResourceOnOpCtx(OperationContext* opCtx);

    // Committing a transaction first changes its state to "Committing*" and writes to the oplog,
    // then it changes the state to "Committed".
    //
    // When a transaction is in "Committing*" state, it's not allowed for other threads to change
    // its state (i.e. abort the transaction), otherwise the on-disk state will diverge from the
    // in-memory state.
    // There are 3 cases where the transaction will be aborted.
    // 1) abortTransaction command. Session check-out mechanism only allows one client to access a
    // transaction.
    // 2) killSession, stepdown, transaction timeout and any thread that aborts the transaction
    // outside of session checkout. They can safely skip the committing transactions.
    // 3) Migration. Should be able to skip committing transactions.
    void _commitTransaction(stdx::unique_lock<stdx::mutex> lk, OperationContext* opCtx);

    const LogicalSessionId _sessionId;

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // Specifies whether the session information needs to be refreshed from storage
    bool _isValid{false};

    // Counter, incremented with each call to invalidate in order to discern invalidations, which
    // happen during refresh
    int _numInvalidations{0};

    // Set to true if incomplete history is detected. For example, when the oplog to a write was
    // truncated because it was too old.
    bool _hasIncompleteHistory{false};

    // Logs the transaction information if it has run slower than the global parameter slowMS. The
    // transaction must be committed or aborted when this function is called.
    void _logSlowTransaction(WithLock wl,
                             const SingleThreadedLockStats* lockStats,
                             TransactionState::StateFlag terminationCause,
                             repl::ReadConcernArgs readConcernArgs);

    // This method returns a string with information about a slow transaction. The format of the
    // logging string produced should match the format used for slow operation logging. A
    // transaction must be completed (committed or aborted) and a valid LockStats reference must be
    // passed in order for this method to be called.
    std::string _transactionInfoForLog(const SingleThreadedLockStats* lockStats,
                                       TransactionState::StateFlag terminationCause,
                                       repl::ReadConcernArgs readConcernArgs);

    // Reports transaction stats for both active and inactive transactions using the provided
    // builder.
    void _reportTransactionStats(WithLock wl,
                                 BSONObjBuilder* builder,
                                 repl::ReadConcernArgs readConcernArgs) const;

    // Caches what is known to be the last written transaction record for the session
    boost::optional<SessionTxnRecord> _lastWrittenSessionRecord;

    // Tracks the last seen txn number for the session and is always >= to the transaction number in
    // the last written txn record. When it is > than that in the last written txn record, this
    // means a new transaction has begun on the session, but it hasn't yet performed any writes.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};

    // Holds transaction resources between network operations.
    boost::optional<TxnResources> _txnResourceStash;

    // Maintains the transaction state and the transition table for legal state transitions.
    TransactionState _txnState;

    // Holds oplog data for operations which have been applied in the current multi-document
    // transaction.  Not used for retryable writes.
    std::vector<repl::ReplOperation> _transactionOperations;

    // Total size in bytes of all operations within the _transactionOperations vector.
    size_t _transactionOperationBytes = 0;

    // For the active txn, tracks which statement ids have been committed and at which oplog
    // opTime. Used for fast retryability check and retrieving the previous write's data without
    // having to scan through the oplog.
    CommittedStatementTimestampMap _activeTxnCommittedStatements;

    // Set in _beginOrContinueTxn and applies to the activeTxn on the session.
    bool _autocommit{true};

    // Set when a snapshot read / transaction begins. Alleviates cache pressure by limiting how long
    // a snapshot will remain open and available. Checked in combination with _txnState to determine
    // whether the transaction should be aborted.
    // This is unset until a transaction begins on the session, and then reset only when new
    // transactions begin.
    boost::optional<Date_t> _transactionExpireDate;

    // The OpTime a speculative transaction is reading from and also the earliest opTime it
    // should wait for write concern for on commit.
    repl::OpTime _speculativeTransactionReadOpTime;

    // This member is only applicable to operations running in a transaction. It is reset when a
    // transaction state resets.
    std::vector<MultikeyPathInfo> _multikeyPathInfo;

    // Tracks metrics for a single multi-document transaction. Not used for retryable writes.
    boost::optional<SingleTransactionStats> _singleTransactionStats;
};

}  // namespace mongo
