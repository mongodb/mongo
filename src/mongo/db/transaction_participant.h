/*
 *    Copyright (C) 2018 MongoDB, Inc.
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
#include <iostream>
#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session.h"
#include "mongo/db/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_metrics_observer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class OperationContext;

extern AtomicInt32 transactionLifetimeLimitSeconds;

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
        /**
         * Stashes transaction state from 'opCtx' in the newly constructed TxnResources.
         * Ephemerally holds the Client lock associated with opCtx.
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
         * Ephemerally holds the Client lock associated with opCtx.
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
        boost::optional<TxnResources> _txnResources;
        OperationContext* _opCtx;
    };

    static TransactionParticipant* get(OperationContext* opCtx);

    /**
     * This should only be used when session was obtained without checking it out.
     */
    static TransactionParticipant* getFromNonCheckedOutSession(Session* session);

    TransactionParticipant() = default;

    static boost::optional<TransactionParticipant>& get(Session* session);
    static void create(Session* session);

    class StateMachine {
    public:
        friend class TransactionParticipant;

        // Note: We must differentiate the 'committed/aborted' and 'committed/aborted after prepare'
        // states, because it is illegal to receive, for example, a prepare request after a
        // commit/abort if no prepare was received before the commit/abort.
        enum class State {
            kUnprepared,
            kAborted,
            kCommitted,
            kWaitingForDecision,
            kAbortedAfterPrepare,
            kCommittedAfterPrepare,

            // The state machine transitions to this state when a message that is considered illegal
            // to receive in a particular state is received. This indicates either a byzantine
            // message, or that the transition table does not accurately reflect an asynchronous
            // network.
            kBroken,
        };

        // State machine inputs
        enum class Event {
            kRecvPrepare,
            kVoteCommitRejected,
            kRecvAbort,
            kRecvCommit,
        };

        // State machine outputs
        enum class Action {
            kNone,
            kPrepare,
            kAbort,
            kCommit,
            kSendCommitAck,
            kSendAbortAck,
        };

        Action onEvent(Event e);

        State state() {
            return _state;
        }

    private:
        struct Transition {
            Transition() : action(Action::kNone) {}
            Transition(Action action) : action(action) {}
            Transition(Action action, State state) : action(action), nextState(state) {}

            Action action;
            boost::optional<State> nextState;
        };

        static const std::map<State, std::map<Event, Transition>> transitionTable;
        State _state{State::kUnprepared};
    };

    /**
     * Called for speculative transactions to fix the optime of the snapshot to read from.
     */
    void setSpeculativeTransactionOpTimeToLastApplied(OperationContext* opCtx);

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

    SingleTransactionStats getSingleTransactionStats() const {
        return _transactionMetricsObserver.getSingleTransactionStats();
    }

    repl::OpTime getSpeculativeTransactionReadOpTimeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _speculativeTransactionReadOpTime;
    }

    repl::OpTime getPrepareOpTime() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _prepareOpTime;
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

    std::string transactionInfoForLogForTest(const SingleThreadedLockStats* lockStats,
                                             bool committed,
                                             repl::ReadConcernArgs readConcernArgs) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        TransactionState::StateFlag terminationCause =
            committed ? TransactionState::kCommitted : TransactionState::kAborted;
        return _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
    }

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
    * Aborts the transaction and releases transaction resources when we have the Session checked
    * out and 'opCtx' owns the transaction resources.
     */
    void abortActiveTransaction(OperationContext* opCtx);

    /*
     * If the transaction is prepared, stash its resources. If not, it's the same as
     * abortActiveTransaction.
     */
    void abortActiveUnpreparedOrStashPreparedTransaction(OperationContext* opCtx);

    void addMultikeyPathInfo(MultikeyPathInfo info) {
        _multikeyPathInfo.push_back(std::move(info));
    }

    const std::vector<MultikeyPathInfo>& getMultikeyPathInfo() const {
        return _multikeyPathInfo;
    }

    /**
     * Starts a new transaction, or continues an already active transaction.
     *
     * The 'autocommit' argument represents the value of the field given in the original client
     * request. If it is boost::none, no autocommit parameter was passed into the request. Every
     * operation that is part of a multi statement transaction must specify 'autocommit=false'.
     * 'startTransaction' represents the value of the field given in the original client request,
     * and indicates whether this operation is the beginning of a multi-statement transaction.
     *
     * Throws an exception if:
     *      - The values of 'autocommit' and/or 'startTransaction' are inconsistent with the current
     *        state of the transaction.
     */
    void beginOrContinue(TxnNumber txnNumber,
                         boost::optional<bool> autocommit,
                         boost::optional<bool> startTransaction);

    static Status isValid(StringData dbName, StringData cmdName);

    void transitionToPreparedforTest() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _txnState.transitionTo(lk, TransactionState::kPrepared);
    }

    /**
     * Checks to see if the txnNumber changed in the parent session and perform the necessary
     * cleanup.
     */
    void checkForNewTxnNumber();

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

    // Committing a transaction first changes its state to "Committing*" and writes to the oplog,
    // then it changes the state to "Committed".
    //
    // When a transaction is in "Committing" state, it's not allowed for other threads to change
    // its state (i.e. abort the transaction), otherwise the on-disk state will diverge from the
    // in-memory state.
    // There are 3 cases where the transaction will be aborted.
    // 1) abortTransaction command. Session check-out mechanism only allows one client to access a
    // transaction.
    // 2) killSession, stepdown, transaction timeout and any thread that aborts the transaction
    // outside of session checkout. They can safely skip the committing transactions.
    // 3) Migration. Should be able to skip committing transactions.
    void _commitTransaction(stdx::unique_lock<stdx::mutex> lk, OperationContext* opCtx);

    // Stash transaction resources.
    void _stashActiveTransaction(WithLock, OperationContext* opCtx);

    // Abort the transaction if it's in one of the expected states and clean up the transaction
    // states associated with the opCtx.
    void _abortActiveTransaction(WithLock,
                                 OperationContext* opCtx,
                                 TransactionState::StateSet expectedStates);

    // Releases stashed transaction resources to abort the transaction on the session.
    void _abortTransactionOnSession(WithLock);

    // Clean up the transaction resources unstashed on operation context.
    void _cleanUpTxnResourceOnOpCtx(WithLock wl, OperationContext* opCtx);

    // Checks if the current transaction number of this transaction still matches with the
    // parent session as well as the transaction number of the current operation context.
    void _checkIsActiveTransaction(WithLock,
                                   const TxnNumber& requestTxnNumber,
                                   bool checkAbort) const;

    // Checks if the command can be run on this transaction based on the state of the transaction.
    void _checkIsCommandValidWithTxnState(WithLock,
                                          OperationContext* opCtx,
                                          const std::string& cmdName);

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
    // builder.  The lock may be either a lock on _mutex or a lock on _metricsMutex.
    void _reportTransactionStats(WithLock wl,
                                 BSONObjBuilder* builder,
                                 repl::ReadConcernArgs readConcernArgs) const;

    void _updateState(WithLock wl, const Session::RefreshState& newState);

    // Bumps up the transaction number of this transaction and perform the necessary cleanup.
    void _setNewTxnNumber(WithLock wl, const TxnNumber& txnNumber);

    // Attempt to begin or retry a retryable write at the given transaction number.
    void _beginOrContinueRetryableWrite(WithLock wl, TxnNumber txnNumber);

    // Attempt to begin a new multi document transaction at the given transaction number.
    void _beginMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber);

    // Attempt to continue an in-progress multi document transaction at the given transaction
    // number.
    void _continueMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber);

    // Returns the session that this transaction belongs to.
    const Session* _getSession() const;
    Session* _getSession();

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // Holds transaction resources between network operations.
    boost::optional<TxnResources> _txnResourceStash;

    // Maintains the transaction state and the transition table for legal state transitions.
    TransactionState _txnState;

    StateMachine _stateMachine;

    // Holds oplog data for operations which have been applied in the current multi-document
    // transaction.
    std::vector<repl::ReplOperation> _transactionOperations;

    // Total size in bytes of all operations within the _transactionOperations vector.
    size_t _transactionOperationBytes = 0;

    // This is the txnNumber that this transaction is actively working on. It can be different from
    // the current txnNumber of the parent session (since it can be changed in couple of ways, like
    // migration). In which case, it should make the necessary steps to also bump this number, like
    // aborting the current transaction.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};

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

    std::vector<MultikeyPathInfo> _multikeyPathInfo;

    // Remembers the refresh count this object has read from Session.
    long long _lastStateRefreshCount{0};

    // Protects _transactionMetricsObserver.  The concurrency rules are that const methods on
    // _transactionMetricsObserver may be called under either _mutex or _metricsMutex, but for
    // non-const methods, both mutexes must be held, with _mutex being taken before _metricsMutex.
    // No other locks, particularly including the Client lock, may be taken while holding
    // _metricsMutex.
    mutable stdx::mutex _metricsMutex;

    // Tracks and updates transaction metrics upon the appropriate transaction event.
    TransactionMetricsObserver _transactionMetricsObserver;
};

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::State& state) {
    using State = TransactionParticipant::StateMachine::State;
    switch (state) {
        // clang-format off
        case State::kUnprepared:                return sb << "Unprepared";
        case State::kAborted:                   return sb << "Aborted";
        case State::kCommitted:                 return sb << "Committed";
        case State::kWaitingForDecision:        return sb << "WaitingForDecision";
        case State::kAbortedAfterPrepare:       return sb << "AbortedAfterPrepare";
        case State::kCommittedAfterPrepare:     return sb << "CommittedAfterPrepare";
        case State::kBroken:                    return sb << "Broken";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::State& state) {
    StringBuilder sb;
    sb << state;
    return os << sb.str();
}

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::Event& event) {
    using Event = TransactionParticipant::StateMachine::Event;
    switch (event) {
        // clang-format off
        case Event::kRecvPrepare:               return sb << "RecvPrepare";
        case Event::kVoteCommitRejected:        return sb << "VoteCommitRejected";
        case Event::kRecvAbort:                 return sb << "RecvAbort";
        case Event::kRecvCommit:                return sb << "RecvCommit";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::Event& event) {
    StringBuilder sb;
    sb << event;
    return os << sb.str();
}

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::Action& action) {
    using Action = TransactionParticipant::StateMachine::Action;
    switch (action) {
        // clang-format off
        case Action::kNone:                     return sb << "None";
        case Action::kPrepare:                  return sb << "Prepare";
        case Action::kAbort:                    return sb << "Abort";
        case Action::kCommit:                   return sb << "Commit";
        case Action::kSendCommitAck:            return sb << "SendCommitAck";
        case Action::kSendAbortAck:             return sb << "SendAbortAck";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::Action& action) {
    StringBuilder sb;
    sb << action;
    return os << sb.str();
}

}  // namespace mongo
