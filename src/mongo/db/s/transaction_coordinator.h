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

#include <vector>

#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {

class TransactionCoordinatorMetricsObserver;

/**
 * State machine, which implements the two-phase commit protocol for a specific transaction,
 * identified by lsid + txnNumber + txnRetryCounter.
 *
 * The lifetime of a coordinator starts with a construction and ends with the `onCompletion()`
 * future getting signaled. It is illegal to destroy a coordinator without waiting for
 * `onCompletion()`.
 */
class TransactionCoordinator {
    TransactionCoordinator(const TransactionCoordinator&) = delete;
    TransactionCoordinator& operator=(const TransactionCoordinator&) = delete;

public:
    /**
     * The two-phase commit steps.
     */
    enum class Step {
        kInactive,
        kWritingParticipantList,
        kWaitingForVotes,
        kWritingDecision,
        kWaitingForDecisionAcks,
        kDeletingCoordinatorDoc,
    };

    /**
     * Instantiates a new TransactionCoordinator for the specified lsid + txnNumber +
     * txnRetryCounter and gives it a 'scheduler' to use for any asynchronous tasks it spawns.
     *
     * If the 'coordinateCommitDeadline' parameter is specified, a timed task will be scheduled to
     * cause the coordinator to be put in a cancelled state, if runCommit is not eventually
     * received.
     */
    TransactionCoordinator(OperationContext* operationContext,
                           const LogicalSessionId& lsid,
                           const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                           std::unique_ptr<txn::AsyncWorkScheduler> scheduler,
                           Date_t deadline);

    ~TransactionCoordinator();

    /**
     * The first time this is called, it asynchronously begins the two-phase commit process for the
     * transaction that this coordinator is responsible for.
     *
     * Subsequent calls will not re-run the commit process.
     */
    void runCommit(OperationContext* opCtx, std::vector<ShardId> participantShards);

    /**
     * To be used to continue coordinating a transaction on step up.
     */
    void continueCommit(const txn::TransactionCoordinatorDocument& doc);

    /**
     * Gets a Future that will contain the decision that the coordinator reaches. Note that this
     * will never be signaled unless runCommit has been called.
     */
    SharedSemiFuture<txn::CommitDecision> getDecision() const;

    /**
     * Returns a future which can be listened on for when all the asynchronous activity spawned by
     * this coordinator has completed. It will always eventually be set and once set it is safe to
     * dispose of the TransactionCoordinator object.
     */
    SharedSemiFuture<txn::CommitDecision> onCompletion();

    /**
     * If runCommit has not yet been called, this will transition this coordinator object to
     * the 'done' state, effectively making it impossible for two-phase commit to be run for this
     * coordinator.
     *
     * Called when a transaction with a higher transaction number is received on this session or
     * when the transaction coordinator service is shutting down.
     */
    void cancelIfCommitNotYetStarted();

    TxnRetryCounter getTxnRetryCounterForTest() const {
        return *_txnNumberAndRetryCounter.getTxnRetryCounter();
    }

    /**
     * Returns the TransactionCoordinatorMetricsObserver for this TransactionCoordinator.
     */
    const TransactionCoordinatorMetricsObserver& getMetricsObserverForTest() {
        return *_transactionCoordinatorMetricsObserver;
    }

    void reportState(BSONObjBuilder& parent) const;
    std::string toString(Step step) const;

    Step getStep() const;

private:
    void _updateAssociatedClient(Client* client);

    bool _reserveKickOffCommitPromise();

    /**
     * Helper for handling errors that occur during either phase of commit coordination.
     */
    void _done(Status s);

    /**
     * Logs the diagnostic string for a commit coordination.
     */
    void _logSlowTwoPhaseCommit(const txn::CoordinatorCommitDecision& decision);

    /**
     * Builds the diagnostic string for a commit coordination.
     */
    std::string _twoPhaseCommitInfoForLog(const txn::CoordinatorCommitDecision& decision) const;

    // Shortcut to the service context under which this coordinator runs
    ServiceContext* const _serviceContext;

    // The lsid + txnNumber + txnRetryCounter that this coordinator is coordinating.
    const LogicalSessionId _lsid;
    const TxnNumberAndRetryCounter _txnNumberAndRetryCounter;

    // Scheduler and context wrapping all asynchronous work dispatched by this coordinator
    std::unique_ptr<txn::AsyncWorkScheduler> _scheduler;

    // Scheduler used for the persist participants + prepare part of the 2PC sequence and
    // interrupted separately from the rest of the chain in order to allow the clean-up tasks
    // (running on _scheduler to still be able to execute).
    std::unique_ptr<txn::AsyncWorkScheduler> _sendPrepareScheduler;

    // Protects the state below
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TransactionCoordinator::_mutex");

    // Tracks which step of the 2PC coordination is currently (or was most recently) executing
    Step _step{Step::kInactive};

    // Promise/future pair which will be signaled when the coordinator has completed
    bool _kickOffCommitPromiseSet{false};
    Promise<void> _kickOffCommitPromise;

    // The state below gets populated sequentially as the coordinator advances through the 2 phase
    // commit stages. Each of these fields is set only once for the lifetime of a coordinator and
    // after that never changes.
    //
    // If the coordinator is canceled before commit is requested, none of these fiends will be set

    // Set when the coordinator has been asked to coordinate commit
    boost::optional<txn::ParticipantsList> _participants;
    bool _participantsDurable{false};

    // Set when the coordinator has heard back from all the participants and reached a decision, but
    // hasn't yet persisted it
    boost::optional<txn::CoordinatorCommitDecision> _decision;

    // Set when the coordinator has durably persisted `_decision` to the `config.coordinators`
    // collection
    bool _decisionDurable{false};
    SharedPromise<txn::CommitDecision> _decisionPromise;

    // A list of all promises corresponding to futures that were returned to callers of
    // onCompletion.
    SharedPromise<txn::CommitDecision> _completionPromise;

    // Store as unique_ptr to avoid a circular dependency between the TransactionCoordinator and
    // the TransactionCoordinatorMetricsObserver.
    std::unique_ptr<TransactionCoordinatorMetricsObserver> _transactionCoordinatorMetricsObserver;

    // The deadline for the TransactionCoordinator to reach a decision
    Date_t _deadline;
};

}  // namespace mongo
