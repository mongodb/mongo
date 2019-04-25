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

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_metrics_observer.h"
#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/str.h"

namespace mongo {

class OperationContext;

/**
 * Read timestamp to be used for a speculative transaction.  For transactions with read
 * concern level specified as 'snapshot', we will use 'kAllCommitted' which ensures a snapshot
 * with no 'holes'; that is, it is a state of the system that could be reconstructed from
 * the oplog.  For transactions with read concern level specified as 'local' or 'majority',
 * we will use 'kNoTimestamp' which gives us the most recent snapshot.  This snapshot may
 * reflect oplog 'holes' from writes earlier than the last applied write which have not yet
 * completed.  Using 'kNoTimestamp' ensures that transactions with mode 'local' are always able to
 * read writes from earlier transactions with mode 'local' on the same connection.
 */
enum class SpeculativeTransactionOpTime {
    kNoTimestamp,
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
 * This class maintains the state of a transaction running on a server session. It can only exist as
 * a decoration on the Session object and its state can only be modified by the thread which has the
 * session checked-out.
 *
 * Its methods are split in two groups with distinct read/write and concurrency control rules. See
 * the comments below for more information.
 */
class TransactionParticipant {
    TransactionParticipant(const TransactionParticipant&) = delete;
    TransactionParticipant& operator=(const TransactionParticipant&) = delete;

    struct PrivateState;
    struct ObservableState;

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
            kAbortedWithPrepare = 1 << 7,
            kExecutedRetryableWrite = 1 << 8,
        };

        using StateSet = int;
        bool isInSet(StateSet stateSet) const {
            return _state & stateSet;
        }

        /**
         * Transitions the session from the current state to the new state. If transition validation
         * is not relaxed, invariants if the transition is illegal.
         */
        enum class TransitionValidation { kValidateTransition, kRelaxTransitionValidation };
        void transitionTo(
            StateFlag newState,
            TransitionValidation shouldValidate = TransitionValidation::kValidateTransition);

        bool inMultiDocumentTransaction() const {
            return _state == kInProgress || _state == kPrepared;
        }

        bool isNone() const {
            return _state == kNone;
        }

        bool isInProgress() const {
            return _state == kInProgress;
        }

        bool isPrepared() const {
            return _state == kPrepared;
        }

        bool isCommittingWithoutPrepare() const {
            return _state == kCommittingWithoutPrepare;
        }

        bool isCommittingWithPrepare() const {
            return _state == kCommittingWithPrepare;
        }

        bool isCommitted() const {
            return _state == kCommitted;
        }

        bool isAborted() const {
            return _state == kAbortedWithPrepare || _state == kAbortedWithoutPrepare;
        }

        bool hasExecutedRetryableWrite() const {
            return _state == kExecutedRetryableWrite;
        }

        bool isInRetryableWriteMode() const {
            return _state == kNone || _state == kExecutedRetryableWrite;
        }

        std::string toString() const {
            return toString(_state);
        }

        static std::string toString(StateFlag state);

        // An optional promise that is non-none while the participant is in prepare. The promise is
        // fulfilled and the optional is reset when the participant transitions out of prepare.
        boost::optional<SharedPromise<void>> _exitPreparePromise;

    private:
        static bool _isLegalTransition(StateFlag oldState, StateFlag newState);

        // Private because any modifications should go through transitionTo.
        StateFlag _state = kNone;
    };

public:
    static inline MutableObeserverRegistry<int32_t> observeTransactionLifetimeLimitSeconds;

    /**
     * Holds state for a snapshot read or multi-statement transaction in between network
     * operations.
     */
    class TxnResources {
    public:
        enum class StashStyle { kPrimary, kSecondary };

        /**
         * Stashes transaction state from 'opCtx' in the newly constructed TxnResources.
         * Caller must hold the Client lock associated with opCtx, attested by WithLock.
         */
        TxnResources(WithLock, OperationContext* opCtx, StashStyle stashStyle) noexcept;
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
     *  An RAII object that stashes the recovery unit from the `opCtx` onto the stack and keeps
     *  using the same locker of `opCtx`. The locker opts out of two-phase locking of the
     *  current WUOW. At destruction it unstashes the recovery unit back onto the `opCtx` and
     *  restores the locker state relevant to the original WUOW.
     */
    class SideTransactionBlock {
    public:
        SideTransactionBlock(OperationContext* opCtx);
        ~SideTransactionBlock();

    private:
        Locker::WUOWLockSnapshot _WUOWLockSnapshot;
        std::unique_ptr<RecoveryUnit> _recoveryUnit;
        WriteUnitOfWork::RecoveryUnitState _ruState;
        OperationContext* _opCtx;
    };

    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;

    static const BSONObj kDeadEndSentinel;

    /**
     * Class used by observers to examine the state of a TransactionParticipant.
     */
    class Observer {
    public:
        explicit Observer(const ObservableSession& session);

        /**
         * Returns the currently active transaction number on this participant.
         */
        TxnNumber getActiveTxnNumber() const {
            return o().activeTxnNumber;
        }

        /**
         * Returns the op time of the last committed write for this session and transaction. If no
         * write has completed yet, returns an empty timestamp.
         */
        repl::OpTime getLastWriteOpTime() const {
            return o().lastWriteOpTime;
        }

        /**
         * Returns the prepare op time that was selected for the transaction, which can be Null if
         * the transaction is not prepared.
         */
        repl::OpTime getPrepareOpTime() const {
            return o().prepareOpTime;
        }

        /**
         * Returns whether the transaction has exceeded its expiration time.
         */
        bool expiredAsOf(Date_t when) const;

        /**
         * Returns whether we are in a multi-document transaction, which means we have an active
         * transaction which has autocommit:false and has not been committed or aborted. It is
         * possible that the current transaction is stashed onto the stack via a
         * `SideTransactionBlock`.
         */
        bool inMultiDocumentTransaction() const {
            return o().txnState.inMultiDocumentTransaction();
        };

        bool transactionIsCommitted() const {
            return o().txnState.isCommitted();
        }

        bool transactionIsAborted() const {
            return o().txnState.isAborted();
        }

        bool transactionIsPrepared() const {
            return o().txnState.isPrepared();
        }

        /**
         * Returns true if we are in an active multi-document transaction or if the transaction has
         * been aborted. This is used to cover the case where a transaction has been aborted, but
         * the OperationContext state has not been cleared yet.
         */
        bool inActiveOrKilledMultiDocumentTransaction() const {
            return o().txnState.inMultiDocumentTransaction() || o().txnState.isAborted();
        }

        /**
         * If this session is holding stashed locks in txnResourceStash, reports the current state
         * of the session using the provided builder.
         */
        BSONObj reportStashedState(OperationContext* opCtx) const;
        void reportStashedState(OperationContext* opCtx, BSONObjBuilder* builder) const;

        /**
         * If this session is not holding stashed locks in txnResourceStash (transaction is active),
         * reports the current state of the session using the provided builder.
         */
        void reportUnstashedState(OperationContext* opCtx, BSONObjBuilder* builder) const;

    protected:
        explicit Observer(TransactionParticipant* tp) : _tp(tp) {}

        const TransactionParticipant::ObservableState& o() const {
            return _tp->_o;
        }

        const LogicalSessionId& _sessionId() const;

        // Reports transaction stats for both active and inactive transactions using the provided
        // builder.
        void _reportTransactionStats(OperationContext* opCtx,
                                     BSONObjBuilder* builder,
                                     repl::ReadConcernArgs readConcernArgs) const;

        TransactionParticipant* _tp;
    };  // class Observer


    /**
     * Class used by a thread that has checked out the TransactionParticipant's session to
     * observe and modify the transaction participant.
     */
    class Participant : public Observer {
    public:
        explicit Participant(OperationContext* opCtx);
        explicit Participant(const SessionToKill& session);

        explicit operator bool() const {
            return _tp;
        }

        /*
         * Blocking method, which loads the transaction state from storage if it has been marked as
         * needing refresh.
         *
         * In order to avoid the possibility of deadlock, this method must not be called while
         * holding a lock.
         */
        void refreshFromStorageIfNeeded(OperationContext* opCtx);

        /**
         * Starts a new transaction (and if the txnNumber is newer aborts any in-progress
         * transaction on the session), or continues an already active transaction.
         *
         * 'autocommit' comes from the 'autocommit' field in the original client request. The only
         * valid values are boost::none (meaning no autocommit was specified) and false (meaning
         * that this is the beginning of a multi-statement transaction).
         *
         * 'startTransaction' comes from the 'startTransaction' field in the original client
         * request. See below for the acceptable values and the meaning of the combinations of
         * autocommit and startTransaction.
         *
         * autocommit = boost::none, startTransaction = boost::none: Means retryable write
         * autocommit = false, startTransaction = boost::none: Means continuation of a
         * multi-statement transaction
         * autocommit = false, startTransaction = true: Means abort whatever transaction is in
         * progress on the session and start a new transaction
         *
         * Any combination other than the ones listed above will invariant since it is expected that
         * the caller has performed the necessary customer input validations.
         *
         * Exceptions of note, which can be thrown are:
         *   - TransactionTooOld - if an attempt is made to start a transaction older than the
         * currently active one or the last one which committed
         *   - PreparedTransactionInProgress - if the transaction is in the prepared state and a new
         * transaction or retryable write is attempted
         *   - NotMaster - if the node is not a primary when this method is called.
         *   - IncompleteTransactionHistory - if an attempt is made to begin a retryable write for a
         * TransactionParticipant that is not in retryable write mode. This is expected behavior if
         * a retryable write has been upgraded to a transaction by the server, which can happen e.g.
         * when updating the shard key.
         */
        void beginOrContinue(OperationContext* opCtx,
                             TxnNumber txnNumber,
                             boost::optional<bool> autocommit,
                             boost::optional<bool> startTransaction);

        /**
         * Used only by the secondary oplog application logic. Similar to 'beginOrContinue' without
         * performing any checks for whether the new txnNumber will start a transaction number in
         * the past.
         */
        void beginOrContinueTransactionUnconditionally(OperationContext* opCtx,
                                                       TxnNumber txnNumber);

        /**
         * If the participant is in prepare, returns a future whose promise is fulfilled when the
         * participant transitions out of prepare.
         *
         * If the participant is not in prepare, returns an immediately ready future.
         *
         * The caller should not wait on the future with the session checked out, since that will
         * prevent the promise from being able to be fulfilled, i.e., will cause a deadlock.
         */
        SharedSemiFuture<void> onExitPrepare() const;

        /**
         * Transfers management of transaction resources from the currently checked-out
         * OperationContext to the Session.
         */
        void stashTransactionResources(OperationContext* opCtx);

        /**
         * Resets the retryable writes state.
         */
        void resetRetryableWriteState(OperationContext* opCtx);

        /**
         * Transfers management of transaction resources from the Session to the currently
         * checked-out OperationContext.
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
         * On a secondary, the "commitOplogEntryOpTime" will be the OpTime of the commitTransaction
         * oplog entry.
         *
         * Throws an exception if the transaction is not prepared or if the 'commitTimestamp' is
         * null.
         */
        void commitPreparedTransaction(OperationContext* opCtx,
                                       Timestamp commitTimestamp,
                                       boost::optional<repl::OpTime> commitOplogEntryOpTime);

        /**
         * Aborts the transaction, if it is not in the "prepared" state.
         */
        void abortTransactionIfNotPrepared(OperationContext* opCtx);

        /*
         * Aborts the transaction, releasing transaction resources.
         */
        void abortActiveTransaction(OperationContext* opCtx);

        /*
         * If the transaction is prepared, stash its resources. If not, it's the same as
         * abortActiveTransaction.
         */
        void abortActiveUnpreparedOrStashPreparedTransaction(OperationContext* opCtx);

        /**
         * Aborts the storage transaction of the prepared transaction on this participant by
         * releasing its resources. Also invalidates the session and the current transaction state.
         * Avoids writing any oplog entries or making any changes to the transaction table since the
         * state for prepared transactions will be re-constituted during replication recovery.
         */
        void abortPreparedTransactionForRollback(OperationContext* opCtx);

        /**
         * Adds a stored operation to the list of stored operations for the current multi-document
         * (non-autocommit) transaction.  It is illegal to add operations when no multi-document
         * transaction is in progress.
         */
        void addTransactionOperation(OperationContext* opCtx, const repl::ReplOperation& operation);

        /**
         * Returns a reference to the stored operations for a completed multi-document
         * (non-autocommit) transaction. "Completed" implies that no more operations will be added
         * to the transaction.  It is legal to call this method only when the transaction state is
         * in progress or committed.
         */
        std::vector<repl::ReplOperation>& retrieveCompletedTransactionOperations(
            OperationContext* opCtx);

        /**
         * Returns an object containing transaction-related metadata to append on responses.
         */
        TxnResponseMetadata getResponseMetadata();

        /**
         * Clears the stored operations for an multi-document (non-autocommit) transaction, marking
         * the transaction as closed.  It is illegal to attempt to add operations to the transaction
         * after this is called.
         */
        void clearOperationsInMemory(OperationContext* opCtx);

        /**
         * Yield or reacquire locks for prepared transactions, used on replication state transition.
         */
        void refreshLocksForPreparedTransaction(OperationContext* opCtx, bool yieldLocks);

        /**
         * May only be called while a multi-document transaction is not committed and adds the
         * multi-key path info to the set of path infos to be updated at commit time.
         */
        void addUncommittedMultikeyPathInfo(MultikeyPathInfo info) {
            invariant(inMultiDocumentTransaction());
            p().multikeyPathInfo.emplace_back(std::move(info));
        }

        /**
         * May only be called while a mutil-document transaction is not committed and returns the
         * path infos which have been added so far.
         */
        const std::vector<MultikeyPathInfo>& getUncommittedMultikeyPathInfos() const {
            invariant(inMultiDocumentTransaction());
            return p().multikeyPathInfo;
        }

        /**
         * Called after a write under the specified transaction completes while the node is a
         * primary and specifies the statement ids which were written. Must be called while the
         * caller is still in the write's WUOW. Updates the on-disk state of the session to match
         * the specified transaction/opTime and keeps the cached state in sync.
         *
         * 'txnState' is 'none' for retryable writes.
         *
         * Throws if the session has been invalidated or the active transaction number doesn't
         * match.
         */
        void onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                         TxnNumber txnNumber,
                                         std::vector<StmtId> stmtIdsWritten,
                                         const repl::OpTime& lastStmtIdWriteOpTime,
                                         Date_t lastStmtIdWriteDate,
                                         boost::optional<DurableTxnStateEnum> txnState,
                                         boost::optional<repl::OpTime> startOpTime);

        /**
         * Called after an entry for the specified session and transaction has been written to the
         * oplog during chunk migration, while the node is still primary. Must be called while the
         * caller is still in the oplog write's WUOW. Updates the on-disk state of the session to
         * match the specified transaction/opTime and keeps the cached state in sync.
         *
         * Throws if the session has been invalidated or the active transaction number is newer than
         * the one specified.
         */
        void onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                         TxnNumber txnNumber,
                                         std::vector<StmtId> stmtIdsWritten,
                                         const repl::OpTime& lastStmtIdWriteOpTime,
                                         Date_t oplogLastStmtIdWriteDate);

        /**
         * Checks whether the given statementId for the specified transaction has already executed
         * and if so, returns the oplog entry which was generated by that write. If the statementId
         * hasn't executed, returns boost::none.
         *
         * Throws if the session has been invalidated or the active transaction number doesn't
         * match.
         */
        boost::optional<repl::OplogEntry> checkStatementExecuted(OperationContext* opCtx,
                                                                 StmtId stmtId) const;

        /**
         * Checks whether the given statementId for the specified transaction has already executed
         * without fetching the oplog entry which was generated by that write.
         *
         * Throws if the session has been invalidated or the active transaction number doesn't
         * match.
         */
        bool checkStatementExecutedNoOplogEntryFetch(StmtId stmtId) const;

        /**
         * Marks the session as requiring refresh. Used when the session state has been modified
         * externally, such as through a direct write to the transactions table.
         */
        void invalidate(OperationContext* opCtx);

        /**
         * Kills the transaction if it is running, ensuring that it releases all resources, even if
         * the transaction is in prepare().  Avoids writing any oplog entries or making any changes
         * to the transaction table.  State for prepared transactions will be re-constituted at
         * startup.  Note that we don't take any active steps to prevent continued use of this
         * TransactionParticipant after shutdown() is called, but we rely on callers to not continue
         * using the TransactionParticipant once we are in shutdown.
         */
        void shutdown(OperationContext* opCtx);

        //
        // Methods for use in C++ unit tests, only. Beware: these methods may not adhere to the
        // concurrency control rules.
        //

        std::string getTransactionInfoForLogForTest(
            OperationContext* opCtx,
            const SingleThreadedLockStats* lockStats,
            bool committed,
            const repl::ReadConcernArgs& readConcernArgs) const {

            TerminationCause terminationCause =
                committed ? TerminationCause::kCommitted : TerminationCause::kAborted;
            return _transactionInfoForLog(opCtx, lockStats, terminationCause, readConcernArgs);
        }

        SingleTransactionStats getSingleTransactionStatsForTest() const {
            return o().transactionMetricsObserver.getSingleTransactionStats();
        }

        std::vector<repl::ReplOperation> getTransactionOperationsForTest() const {
            return p().transactionOperations;
        }

        repl::OpTime getSpeculativeTransactionReadOpTimeForTest() const {
            return p().speculativeTransactionReadOpTime;
        }

        const Locker* getTxnResourceStashLockerForTest() const {
            invariant(o().txnResourceStash);
            return o().txnResourceStash->locker();
        }

        void transitionToPreparedforTest(OperationContext* opCtx, repl::OpTime prepareOpTime) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).prepareOpTime = prepareOpTime;
            o(lk).txnState.transitionTo(TransactionState::kPrepared);
        }

        void transitionToCommittingWithPrepareforTest(OperationContext* opCtx) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).txnState.transitionTo(TransactionState::kCommittingWithPrepare);
        }

        void transitionToAbortedWithoutPrepareforTest(OperationContext* opCtx) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).txnState.transitionTo(TransactionState::kAbortedWithoutPrepare);
        }

        void transitionToAbortedWithPrepareforTest(OperationContext* opCtx) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).txnState.transitionTo(TransactionState::kAbortedWithPrepare);
        }

    private:
        boost::optional<repl::OpTime> _checkStatementExecuted(StmtId stmtId) const;

        UpdateRequest _makeUpdateRequest(const repl::OpTime& newLastWriteOpTime,
                                         Date_t newLastWriteDate,
                                         boost::optional<DurableTxnStateEnum> newState,
                                         boost::optional<repl::OpTime> startOpTime) const;

        void _registerUpdateCacheOnCommit(OperationContext* opCtx,
                                          std::vector<StmtId> stmtIdsWritten,
                                          const repl::OpTime& lastStmtIdWriteTs);

        // Called for speculative transactions to fix the optime of the snapshot to read from.
        void _setSpeculativeTransactionOpTime(OperationContext* opCtx,
                                              SpeculativeTransactionOpTime opTimeChoice);


        // Like _setSpeculativeTransactionOpTime, but caller chooses timestamp of snapshot
        // explicitly.
        // It is up to the caller to ensure that Timestamp is greater than or equal to the
        // all-committed optime before calling this method (e.g. by calling
        // ReplCoordinator::waitForOpTimeForRead).
        void _setSpeculativeTransactionReadTimestamp(OperationContext* opCtx, Timestamp timestamp);

        // Finishes committing the multi-document transaction after the storage-transaction has been
        // committed, the oplog entry has been inserted into the oplog, and the transactions table
        // has been updated.
        void _finishCommitTransaction(OperationContext* opCtx);

        // Commits the storage-transaction on the OperationContext.
        //
        // This should be called *without* the Client being locked.
        void _commitStorageTransaction(OperationContext* opCtx);

        // Stash transaction resources.
        void _stashActiveTransaction(OperationContext* opCtx);

        // Abort the transaction if it's in one of the expected states and clean up the transaction
        // states associated with the opCtx.
        void _abortActiveTransaction(OperationContext* opCtx,
                                     TransactionState::StateSet expectedStates);

        // Releases stashed transaction resources to abort the transaction on the session.
        void _abortTransactionOnSession(OperationContext* opCtx);

        // Clean up the transaction resources unstashed on operation context.
        void _cleanUpTxnResourceOnOpCtx(OperationContext* opCtx, TerminationCause terminationCause);

        // Checks if the command can be run on this transaction based on the state of the
        // transaction.
        void _checkIsCommandValidWithTxnState(const TxnNumber& requestTxnNumber,
                                              const std::string& cmdName) const;

        // Logs the transaction information if it has run slower than the global parameter slowMS.
        // The transaction must be committed or aborted when this function is called.
        void _logSlowTransaction(OperationContext* opCtx,
                                 const SingleThreadedLockStats* lockStats,
                                 TerminationCause terminationCause,
                                 repl::ReadConcernArgs readConcernArgs);

        // This method returns a string with information about a slow transaction. The format of the
        // logging string produced should match the format used for slow operation logging. A
        // transaction must be completed (committed or aborted) and a valid LockStats reference must
        // be passed in order for this method to be called.
        std::string _transactionInfoForLog(OperationContext* opCtx,
                                           const SingleThreadedLockStats* lockStats,
                                           TerminationCause terminationCause,
                                           repl::ReadConcernArgs readConcernArgs) const;

        // Bumps up the transaction number of this transaction and perform the necessary cleanup.
        void _setNewTxnNumber(OperationContext* opCtx, const TxnNumber& txnNumber);

        // Attempt to begin or retry a retryable write at the given transaction number.
        void _beginOrContinueRetryableWrite(OperationContext* opCtx, TxnNumber txnNumber);

        // Attempt to begin a new multi document transaction at the given transaction number.
        void _beginMultiDocumentTransaction(OperationContext* opCtx, TxnNumber txnNumber);

        // Attempt to continue an in-progress multi document transaction at the given transaction
        // number.
        void _continueMultiDocumentTransaction(OperationContext* opCtx, TxnNumber txnNumber);

        // Helper that invalidates the session state and activeTxnNumber. Also resets the single
        // transaction stats because the session is no longer valid.
        void _invalidate(WithLock);

        // Helper that resets the retryable writes state.
        void _resetRetryableWriteState();

        // Helper that resets the transactional state. This is used when aborting a transaction,
        // invalidating a transaction, or starting a new transaction.
        void _resetTransactionState(WithLock wl, TransactionState::StateFlag state);

        // Helper that updates ServerTransactionsMetrics once a transaction commits.
        void _updateTxnMetricsOnCommit(OperationContext* opCtx, bool isCommittingWithPrepare);

        // Releases the resources held in *o().txnResources to the operation context.
        // o().txnResources must be engaged prior to calling this.
        void _releaseTransactionResourcesToOpCtx(OperationContext* opCtx);

        TransactionParticipant::PrivateState& p() {
            return _tp->_p;
        }
        const TransactionParticipant::PrivateState& p() const {
            return _tp->_p;
        }
        TransactionParticipant::ObservableState& o(WithLock) {
            return _tp->_o;
        }
        using Observer::o;
    };  // class Participant

    static Participant get(OperationContext* opCtx) {
        return Participant(opCtx);
    }

    static Participant get(const SessionToKill& session) {
        return Participant(session);
    }

    static Observer get(const ObservableSession& osession) {
        return Observer(osession);
    }


    /**
     * Returns the timestamp of the oldest oplog entry written across all open transactions, at the
     * time of the stable timestamp. Returns boost::none if there are no active transactions, or an
     * error if it fails.
     */
    static StorageEngine::OldestActiveTransactionTimestampResult getOldestActiveTimestamp(
        Timestamp stableTimestamp);

    /**
     * Append a no-op to the oplog, for cases where we haven't written in this unit of work but
     * want to await a write concern.
     */
    static void performNoopWrite(OperationContext* opCtx, StringData msg);

    TransactionParticipant() = default;
    ~TransactionParticipant() = default;

private:
    /**
     * Reserves a slot in the oplog with an open storage-transaction while it is alive. Reserves the
     * slot at construction. Aborts the storage-transaction and releases the oplog slot at
     * destruction.
     */
    class OplogSlotReserver {
    public:
        OplogSlotReserver(OperationContext* opCtx, int numSlotsToReserve = 1);
        ~OplogSlotReserver();

        /**
         * Returns the latest oplog slot reserved at construction.
         */
        OplogSlot getLastSlot() {
            invariant(!_oplogSlots.empty());
            invariant(!_oplogSlots.back().isNull());
            return getSlots().back();
        }

        std::vector<OplogSlot>& getSlots() {
            invariant(!_oplogSlots.empty());
            invariant(!_oplogSlots.back().isNull());
            return _oplogSlots;
        }

    private:
        OperationContext* _opCtx;
        std::unique_ptr<RecoveryUnit> _recoveryUnit;
        std::vector<OplogSlot> _oplogSlots;
    };

    friend std::ostream& operator<<(std::ostream& s, const TransactionState& txnState) {
        return (s << txnState.toString());
    }

    friend StringBuilder& operator<<(StringBuilder& s, const TransactionState& txnState) {
        return (s << txnState.toString());
    }

    /**
     * State in this struct may be read by methods of Observer or Participant, and may be written by
     * methods of Participant when they acquire the lock on the opCtx's Client. Access this inside
     * Observer and Participant using the private o() method for reading and (Participant only) the
     * o(WithLock) method for writing.
     */
    struct ObservableState {
        // Holds transaction resources between network operations.
        boost::optional<TxnResources> txnResourceStash;

        // Maintains the transaction state and the transition table for legal state transitions.
        TransactionState txnState;

        // Tracks the last seen txn number for the session and is always >= to the transaction
        // number in the last written txn record. When it is > than that in the last written txn
        // record, this means a new transaction has begun on the session, but it hasn't yet
        // performed any writes.
        TxnNumber activeTxnNumber{kUninitializedTxnNumber};

        // Caches what is known to be the last optime written for the active transaction.
        repl::OpTime lastWriteOpTime;

        // Set when a snapshot read / transaction begins. Alleviates cache pressure by limiting how
        // long a snapshot will remain open and available. Checked in combination with _txnState to
        // determine whether the transaction should be aborted.  This is unset until a transaction
        // begins on the session, and then reset only when new transactions begin.
        boost::optional<Date_t> transactionExpireDate;

        // Track the prepareOpTime, the OpTime of the 'prepare' oplog entry for a transaction.
        repl::OpTime prepareOpTime;

        // Tracks and updates transaction metrics upon the appropriate transaction event.
        TransactionMetricsObserver transactionMetricsObserver;
    } _o;

    /**
     * State in this struct may be read and written by methods of the Participant, only. It may
     * access the struct via the private p() accessor. No further locking is required in methods
     * of the Participant.
     */
    struct PrivateState {
        // Only set if the server is shutting down and it has been ensured that no new requests will
        // be accepted. Ensures that any transaction resources will not be stashed from the
        // operation context onto the transaction participant when the session is checked-in so that
        // locks can automatically get freed.
        bool inShutdown = false;

        // Holds oplog data for operations which have been applied in the current multi-document
        // transaction.
        std::vector<repl::ReplOperation> transactionOperations;

        // Total size in bytes of all operations within the _transactionOperations vector.
        size_t transactionOperationBytes = 0;

        // The autocommit setting of this transaction. Should always be false for multi-statement
        // transaction. Currently only needed for diagnostics reporting.
        boost::optional<bool> autoCommit;

        // The OpTime a speculative transaction is reading from and also the earliest opTime it
        // should wait for write concern for on commit.
        repl::OpTime speculativeTransactionReadOpTime;

        // Contains uncommitted multi-key path info entries which were modified under this
        // transaction so they can be applied to subsequent opreations before the transaction
        // commits
        std::vector<MultikeyPathInfo> multikeyPathInfo;

        //
        // Retryable writes state
        //

        // Specifies whether the session information needs to be refreshed from storage
        bool isValid{false};

        // Set to true if incomplete history is detected. For example, when the oplog to a write was
        // truncated because it was too old.
        bool hasIncompleteHistory{false};

        // For the active txn, tracks which statement ids have been committed and at which oplog
        // opTime. Used for fast retryability check and retrieving the previous write's data without
        // having to scan through the oplog.
        CommittedStatementTimestampMap activeTxnCommittedStatements;
    } _p;
};

}  // namespace mongo
