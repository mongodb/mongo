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
#include <iostream>
#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_metrics_observer.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class OperationContext;

extern AtomicWord<int> transactionLifetimeLimitSeconds;

/**
 * Read timestamp to be used for a speculative transaction.  For transactions with read
 * concern level specified as 'snapshot', we will use 'kAllCommitted' which ensures a snapshot
 * with no 'holes'; that is, it is a state of the system that could be reconstructed from
 * the oplog.  For transactions with read concern level specified as 'local' or 'majority',
 * we will use 'kLastApplied' which gives us the most recent snapshot.  This snapshot may
 * reflect oplog 'holes' from writes earlier than the last applied write which have not yet
 * completed.  Using 'kLastApplied' ensures that transactions with mode 'local' are always able to
 * read writes from earlier transactions with mode 'local' on the same connection.
 */
enum class SpeculativeTransactionOpTime {
    kLastApplied,
    kAllCommitted,
};

/**
 * Reason a transaction was terminated.
 */
enum class TerminationCause {
    kCommitted,
    kAborted,
};

/**
 * A state machine that coordinates a distributed transaction commit with the transaction
 * coordinator.
 */
class TransactionParticipant {
    MONGO_DISALLOW_COPYING(TransactionParticipant);

public:
    /**
     * Holds state for a snapshot read or multi-statement transaction in between network
     * operations.
     */
    class TxnResources {
    public:
        enum class StashStyle { kPrimary, kSecondary, kSideTransaction };

        /**
         * Stashes transaction state from 'opCtx' in the newly constructed TxnResources.
         * Ephemerally holds the Client lock associated with opCtx.
         */
        TxnResources(OperationContext* opCtx, StashStyle stashStyle);
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
         * Ephemerally holds the Client lock associated with opCtx.
         */
        void release(OperationContext* opCtx);

        /**
         * Returns the read concern arguments.
         */
        const repl::ReadConcernArgs& getReadConcernArgs() const {
            return _readConcernArgs;
        }

    private:
        bool _released = false;
        std::unique_ptr<Locker> _locker;
        std::unique_ptr<Locker::LockSnapshot> _lockSnapshot;
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
        boost::optional<TxnResources> _txnResources;
        OperationContext* _opCtx;
    };

    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;

    static const BSONObj kDeadEndSentinel;

    TransactionParticipant();
    ~TransactionParticipant();

    /**
     * Obtains the transaction participant from a session and a syntactic sugar variant, which
     * obtains it from an operation context on which the session has been checked-out.
     */
    static TransactionParticipant* get(OperationContext* opCtx);
    static TransactionParticipant* get(Session* session);

    /**
     * When the server returns a NoSuchTransaction error for a command, it performs a noop write if
     * there is a writeConcern on the command. The TransientTransactionError label is only appended
     * to a NoSuchTransaction response for 'commitTransaction' and 'coordinateCommitTransaction' if
     * there is no writeConcern error. This ensures that if 'commitTransaction' or
     * 'coordinateCommitTransaction' is run with w:majority, then the TransientTransactionError
     * label is only returned if the transaction is not committed on any valid branch of history,
     * so the driver or application can safely retry the entire transaction.
     */
    static void performNoopWriteForNoSuchTransaction(OperationContext* opCtx);

    /**
     * Blocking method, which loads the transaction state from storage if it has been marked as
     * needing refresh.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock.
     */
    void refreshFromStorageIfNeeded();

    /**
     * Starts a new transaction (and if the txnNumber is newer aborts any in-progress transaction on
     * the session), or continues an already active transaction.
     *
     * 'autocommit' comes from the 'autocommit' field in the original client request. The only valid
     * values are boost::none (meaning no autocommit was specified) and false (meaning that this is
     * the beginning of a multi-statement transaction).
     *
     * 'startTransaction' comes from the 'startTransaction' field in the original client request.
     * See below for the acceptable values and the meaning of the combinations of autocommit and
     * startTransaction.
     *
     * autocommit = boost::none, startTransaction = boost::none: Means retryable write
     * autocommit = false, startTransaction = boost::none: Means continuation of a multi-statement
     * transaction
     * autocommit = false, startTransaction = true: Means abort whatever transaction is in progress
     * on the session and start a new transaction
     *
     * Any combination other than the ones listed above will invariant since it is expected that the
     * caller has performed the necessary customer input validations.
     *
     * Exceptions of note, which can be thrown are:
     *   - TransactionTooOld - if attempt is made to start a transaction older than the currently
     * active one or the last one which committed
     *   - PreparedTransactionInProgress - if the transaction is in the prepared state and a new
     * transaction or retryable write is attempted
     */
    void beginOrContinue(TxnNumber txnNumber,
                         boost::optional<bool> autocommit,
                         boost::optional<bool> startTransaction);

    /**
     * Used only by the secondary oplog application logic. Equivalent to 'beginOrContinue(txnNumber,
     * false, true)' without performing any checks for whether the new txnNumber will start a
     * transaction number in the past.
     *
     * NOTE: This method assumes that there are no concurrent users of the transaction since it
     * unconditionally changes the active transaction on the session.
     */
    void beginOrContinueTransactionUnconditionally(TxnNumber txnNumber);

    /**
     * Transfers management of transaction resources from the OperationContext to the Session.
     */
    void stashTransactionResources(OperationContext* opCtx);

    /**
     * Transfers management of transaction resources from the Session to the OperationContext.
     */
    void unstashTransactionResources(OperationContext* opCtx, const std::string& cmdName);

    /**
     * Puts a transaction into a prepared state and returns the prepareTimestamp.
     *
     * On secondary, the "prepareTimestamp" will be given in the oplog.
     */
    Timestamp prepareTransaction(OperationContext* opCtx,
                                 boost::optional<repl::OpTime> prepareOptime);

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
    * On a secondary, the "commitOplogEntryOpTime" will be the OpTime of the commitTransaction oplog
    * entry.
    *
    * Throws an exception if the transaction is not prepared or if the 'commitTimestamp' is null.
    */
    void commitPreparedTransaction(OperationContext* opCtx,
                                   Timestamp commitTimestamp,
                                   boost::optional<repl::OpTime> commitOplogEntryOpTime);

    /**
     * Aborts the transaction outside the transaction, releasing transaction resources.
     *
     * Not called with session checked out.
     */
    void abortArbitraryTransaction();

    /*
    * Aborts the transaction inside the transaction, releasing transaction resources.
    * We're inside the transaction when we have the Session checked out and 'opCtx' owns the
    * transaction resources.
    * Aborts the transaction and releases transaction resources when we have the Session checked
    * out and 'opCtx' owns the transaction resources.
     */
    void abortActiveTransaction(OperationContext* opCtx);

    /*
     * If the transaction is prepared, stash its resources. If not, it's the same as
     * abortActiveTransaction.
     */
    void abortActiveUnpreparedOrStashPreparedTransaction(OperationContext* opCtx);

    /**
     * Aborts the storage transaction of the prepared transaction on this participant by releasing
     * its resources. Also invalidates the session and the current transaction state.
     * Avoids writing any oplog entries or making any changes to the transaction table since the
     * state for prepared transactions will be re-constituted during replication recovery.
     */
    void abortPreparedTransactionForRollback();

    /**
     * Adds a stored operation to the list of stored operations for the current multi-document
     * (non-autocommit) transaction.  It is illegal to add operations when no multi-document
     * transaction is in progress.
     */
    void addTransactionOperation(OperationContext* opCtx, const repl::ReplOperation& operation);

    /**
     * Returns a reference to the stored operations for a completed multi-document (non-autocommit)
     * transaction. "Completed" implies that no more operations will be added to the transaction.
     * It is legal to call this method only when the transaction state is in progress or committed.
     */
    std::vector<repl::ReplOperation>& retrieveCompletedTransactionOperations(
        OperationContext* opCtx);

    /**
     * Clears the stored operations for an multi-document (non-autocommit) transaction, marking
     * the transaction as closed.  It is illegal to attempt to add operations to the transaction
     * after this is called.
     */
    void clearOperationsInMemory(OperationContext* opCtx);

    /**
     * Yield or reacquire locks for prepared transacitons, used on replication state transition.
     */
    void refreshLocksForPreparedTransaction(OperationContext* opCtx, bool yieldLocks);

    /**
     * May only be called while a multi-document transaction is not committed and adds the multi-key
     * path info to the set of path infos to be updated at commit time.
     */
    void addUncommittedMultikeyPathInfo(MultikeyPathInfo info) {
        invariant(inMultiDocumentTransaction());
        _multikeyPathInfo.emplace_back(std::move(info));
    }

    /**
     * May only be called while a mutil-document transaction is not committed and returns the path
     * infos which have been added so far.
     */
    const std::vector<MultikeyPathInfo>& getUncommittedMultikeyPathInfos() const {
        invariant(inMultiDocumentTransaction());
        return _multikeyPathInfo;
    }

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
     * Checks whether the given statementId for the specified transaction has already executed and
     * if so, returns the oplog entry which was generated by that write. If the statementId hasn't
     * executed, returns boost::none.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    boost::optional<repl::OplogEntry> checkStatementExecuted(StmtId stmtId) const;

    /**
     * Checks whether the given statementId for the specified transaction has already executed
     * without fetching the oplog entry which was generated by that write.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    bool checkStatementExecutedNoOplogEntryFetch(StmtId stmtId) const;

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    void invalidate();

    /**
     * Kills the transaction if it is running, ensuring that it releases all resources, even if the
     * transaction is in prepare().  Avoids writing any oplog entries or making any changes to the
     * transaction table.  State for prepared transactions will be re-constituted at startup.
     * Note that we don't take any active steps to prevent continued use of this
     * TransactionParticipant after shutdown() is called, but we rely on callers to not
     * continue using the TransactionParticipant once we are in shutdown.
     */
    void shutdown();

    /**
     * Returns the currently active transaction number on this participant.
     */
    TxnNumber getActiveTxnNumber() const {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        return _activeTxnNumber;
    }

    /**
     * Returns the op time of the last committed write for this session and transaction. If no write
     * has completed yet, returns an empty timestamp.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    repl::OpTime getLastWriteOpTime() const;

    /**
     * Returns the prepare op time that was selected for the transaction, which can be Null if the
     * transaction is not prepared.
     */
    repl::OpTime getPrepareOpTime() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _prepareOpTime;
    }

    /**
     * Returns whether the transaction has exceeded its expiration time.
     */
    bool expired() const;

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

    bool transactionIsPrepared() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _txnState.isPrepared(lk);
    }

    /**
     * Returns true if we are in an active multi-document transaction or if the transaction has
     * been aborted. This is used to cover the case where a transaction has been aborted, but the
     * OperationContext state has not been cleared yet.
     */
    bool inActiveOrKilledMultiDocumentTransaction() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return (_txnState.inMultiDocumentTransaction(lk) || _txnState.isAborted(lk));
    }

    /**
     * If this session is holding stashed locks in _txnResourceStash, reports the current state of
     * the session using the provided builder. Locks the session object's mutex while running.
     */
    BSONObj reportStashedState() const;
    void reportStashedState(BSONObjBuilder* builder) const;

    /**
     * If this session is not holding stashed locks in _txnResourceStash (transaction is active),
     * reports the current state of the session using the provided builder. Locks the session
     * object's mutex while running.
     *
     * If this is called from a thread other than the owner of the opCtx, that thread must be
     * holding the client lock.
     */
    void reportUnstashedState(OperationContext* opCtx, BSONObjBuilder* builder) const;

    //
    // Methods used for unit-testing only
    //

    std::string getTransactionInfoForLogForTest(
        const SingleThreadedLockStats* lockStats,
        bool committed,
        const repl::ReadConcernArgs& readConcernArgs) const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        TerminationCause terminationCause =
            committed ? TerminationCause::kCommitted : TerminationCause::kAborted;
        return _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
    }

    SingleTransactionStats getSingleTransactionStatsForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_metricsMutex);
        return _transactionMetricsObserver.getSingleTransactionStats();
    }

    std::vector<repl::ReplOperation> getTransactionOperationsForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _transactionOperations;
    }

    repl::OpTime getSpeculativeTransactionReadOpTimeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _speculativeTransactionReadOpTime;
    }

    boost::optional<repl::OpTime> getFinishOpTimeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _finishOpTime;
    }

    boost::optional<repl::OpTime> getOldestOplogEntryOpTimeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _oldestOplogEntryOpTime;
    }

    const Locker* getTxnResourceStashLockerForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_txnResourceStash);
        return _txnResourceStash->locker();
    }

    void transitionToPreparedforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kPrepared);
    }

    void transitionToCommittingWithPrepareforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kCommittingWithPrepare);
    }


    void transitionToAbortedWithoutPrepareforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kAbortedWithoutPrepare);
    }

    void transitionToAbortedWithPrepareforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kAbortedWithPrepare);
    }

private:
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
        OperationContext* _opCtx;
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
            kAbortedWithoutPrepare = 1 << 6,
            kAbortedWithPrepare = 1 << 7
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
            return _state == kAbortedWithoutPrepare || _state == kAbortedWithPrepare;
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

    // Shortcut to obtain the id of the session under which this participant runs
    const LogicalSessionId& _sessionId() const;

    // Shortcut to obtain the currently checked-out operation context under this participant runs
    OperationContext* _opCtx() const;

    /**
     * Performing any checks based on the in-memory state of the TransactionParticipant requires
     * that the object is fully in sync with its on-disk representation in the transactions table.
     * This method checks that. The object can be out of sync with the on-disk representation either
     * when it was just created, or after invalidate() was called (which typically happens after a
     * direct write to the transactions table).
     */
    void _checkValid(WithLock) const;

    // Checks that the specified transaction number is the same as the activeTxnNumber. Effectively
    // a check that the caller operates on the transaction it thinks it is operating on.
    void _checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const;

    boost::optional<repl::OpTime> _checkStatementExecuted(StmtId stmtId) const;

    UpdateRequest _makeUpdateRequest(const repl::OpTime& newLastWriteOpTime,
                                     Date_t newLastWriteDate,
                                     boost::optional<DurableTxnStateEnum> newState) const;

    void _registerUpdateCacheOnCommit(std::vector<StmtId> stmtIdsWritten,
                                      const repl::OpTime& lastStmtIdWriteTs);

    // Called for speculative transactions to fix the optime of the snapshot to read from.
    void _setSpeculativeTransactionOpTime(WithLock,
                                          OperationContext* opCtx,
                                          SpeculativeTransactionOpTime opTimeChoice);


    // Like _setSpeculativeTransactionOpTime, but caller chooses timestamp of snapshot explicitly.
    // It is up to the caller to ensure that Timestamp is greater than or equal to the all-committed
    // optime before calling this method (e.g. by calling ReplCoordinator::waitForOpTimeForRead).
    void _setSpeculativeTransactionReadTimestamp(WithLock,
                                                 OperationContext* opCtx,
                                                 Timestamp timestamp);

    // Finishes committing the multi-document transaction after the storage-transaction has been
    // committed, the oplog entry has been inserted into the oplog, and the transactions table has
    // been updated.
    void _finishCommitTransaction(WithLock lk, OperationContext* opCtx);

    // Commits the storage-transaction on the OperationContext.
    //
    // This should be called *without* the mutex being locked.
    void _commitStorageTransaction(OperationContext* opCtx);

    // Stash transaction resources.
    void _stashActiveTransaction(WithLock, OperationContext* opCtx);

    // Abort the transaction if it's in one of the expected states and clean up the transaction
    // states associated with the opCtx.
    void _abortActiveTransaction(stdx::unique_lock<stdx::mutex> lock,
                                 OperationContext* opCtx,
                                 TransactionState::StateSet expectedStates);

    // Releases stashed transaction resources to abort the transaction on the session.
    void _abortTransactionOnSession(WithLock);

    // Clean up the transaction resources unstashed on operation context.
    void _cleanUpTxnResourceOnOpCtx(WithLock wl,
                                    OperationContext* opCtx,
                                    TerminationCause terminationCause);

    // Checks if the current transaction number of this transaction still matches with the
    // parent session as well as the transaction number of the current operation context.
    void _checkIsActiveTransaction(WithLock,
                                   const TxnNumber& requestTxnNumber,
                                   bool checkAbort) const;

    // Checks if the command can be run on this transaction based on the state of the transaction.
    void _checkIsCommandValidWithTxnState(WithLock,
                                          const TxnNumber& requestTxnNumber,
                                          const std::string& cmdName);

    // Logs the transaction information if it has run slower than the global parameter slowMS. The
    // transaction must be committed or aborted when this function is called.
    void _logSlowTransaction(WithLock wl,
                             const SingleThreadedLockStats* lockStats,
                             TerminationCause terminationCause,
                             repl::ReadConcernArgs readConcernArgs);

    // This method returns a string with information about a slow transaction. The format of the
    // logging string produced should match the format used for slow operation logging. A
    // transaction must be completed (committed or aborted) and a valid LockStats reference must be
    // passed in order for this method to be called.
    std::string _transactionInfoForLog(const SingleThreadedLockStats* lockStats,
                                       TerminationCause terminationCause,
                                       repl::ReadConcernArgs readConcernArgs) const;

    // Reports transaction stats for both active and inactive transactions using the provided
    // builder.  The lock may be either a lock on _mutex or a lock on _metricsMutex.
    void _reportTransactionStats(WithLock wl,
                                 BSONObjBuilder* builder,
                                 repl::ReadConcernArgs readConcernArgs) const;

    // Bumps up the transaction number of this transaction and perform the necessary cleanup.
    void _setNewTxnNumber(WithLock wl, const TxnNumber& txnNumber);

    // Attempt to begin or retry a retryable write at the given transaction number.
    void _beginOrContinueRetryableWrite(WithLock wl, TxnNumber txnNumber);

    // Attempt to begin a new multi document transaction at the given transaction number.
    void _beginMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber);

    // Attempt to continue an in-progress multi document transaction at the given transaction
    // number.
    void _continueMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber);

    // Helper that invalidates the session state and activeTxnNumber. Also resets the single
    // transaction stats because the session is no longer valid.
    void _invalidate(WithLock);

    // Helper that resets the retryable writes state.
    void _resetRetryableWriteState(WithLock);

    // Helper that resets the transactional state. This is used when aborting a transaction,
    // invalidating a transaction, or starting a new transaction.
    void _resetTransactionState(WithLock wl, TransactionState::StateFlag state);

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // Holds transaction resources between network operations.
    boost::optional<TxnResources> _txnResourceStash;

    // Maintains the transaction state and the transition table for legal state transitions.
    TransactionState _txnState;

    // Holds oplog data for operations which have been applied in the current multi-document
    // transaction.
    std::vector<repl::ReplOperation> _transactionOperations;

    // Total size in bytes of all operations within the _transactionOperations vector.
    size_t _transactionOperationBytes = 0;

    // Tracks the last seen txn number for the session and is always >= to the transaction number in
    // the last written txn record. When it is > than that in the last written txn record, this
    // means a new transaction has begun on the session, but it hasn't yet performed any writes.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};

    // Caches what is known to be the last optime written for the active transaction.
    repl::OpTime _lastWriteOpTime;

    // Set when a snapshot read / transaction begins. Alleviates cache pressure by limiting how long
    // a snapshot will remain open and available. Checked in combination with _txnState to determine
    // whether the transaction should be aborted.
    // This is unset until a transaction begins on the session, and then reset only when new
    // transactions begin.
    boost::optional<Date_t> _transactionExpireDate;

    // The autoCommit setting of this transaction. Should always be false for multi-statement
    // transaction. Currently only needed for diagnostics reporting.
    boost::optional<bool> _autoCommit;

    // Track the prepareOpTime, the OpTime of the 'prepare' oplog entry for a transaction.
    repl::OpTime _prepareOpTime;

    // The OpTime a speculative transaction is reading from and also the earliest opTime it
    // should wait for write concern for on commit.
    repl::OpTime _speculativeTransactionReadOpTime;

    // Contains uncommitted multi-key path info entries which were modified under this transaction
    // so they can be applied to subsequent opreations before the transaction commits
    std::vector<MultikeyPathInfo> _multikeyPathInfo;

    // Tracks the OpTime of the first oplog entry written by this TransactionParticipant.
    boost::optional<repl::OpTime> _oldestOplogEntryOpTime;

    // Tracks the OpTime of the abort/commit oplog entry associated with this transaction.
    boost::optional<repl::OpTime> _finishOpTime;

    // Protects _transactionMetricsObserver.  The concurrency rules are that const methods on
    // _transactionMetricsObserver may be called under either _mutex or _metricsMutex, but for
    // non-const methods, both mutexes must be held, with _mutex being taken before _metricsMutex.
    // No other locks, particularly including the Client lock, may be taken while holding
    // _metricsMutex.
    mutable stdx::mutex _metricsMutex;

    // Tracks and updates transaction metrics upon the appropriate transaction event.
    TransactionMetricsObserver _transactionMetricsObserver;

    // Only set if the server is shutting down and it has been ensured that no new requests will be
    // accepted. Ensures that any transaction resources will not be stashed from the operation
    // context onto the transaction participant when the session is checked-in so that locks can
    // automatically get freed.
    bool _inShutdown{false};

    //
    // Retryable writes state
    //

    // Specifies whether the session information needs to be refreshed from storage
    bool _isValid{false};

    // Set to true if incomplete history is detected. For example, when the oplog to a write was
    // truncated because it was too old.
    bool _hasIncompleteHistory{false};

    // For the active txn, tracks which statement ids have been committed and at which oplog
    // opTime. Used for fast retryability check and retrieving the previous write's data without
    // having to scan through the oplog.
    CommittedStatementTimestampMap _activeTxnCommittedStatements;
};

}  // namespace mongo
