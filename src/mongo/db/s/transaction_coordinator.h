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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class TransactionCoordinatorMetricsObserver;

/**
 * State machine, which implements the two-phase commit protocol for a specific transaction,
 * identified by lsid + txnNumber + txnRetryCounter.
 *
 * The lifetime of a coordinator starts with a construction, followed by calling `start()` to
 * kick off asynchronous operations, and finally ends with the `onCompletion()` future getting
 * signaled.
 */
class TransactionCoordinator : public std::enable_shared_from_this<TransactionCoordinator> {
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
        kWritingEndOfTransaction,
        kDeletingCoordinatorDoc,

        kLastStep = kDeletingCoordinatorDoc
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

    /*
     * After constructing a TransactionCoordinator, calling start() begins its asynchronous
     * operations.
     */
    void start(OperationContext* operationContext);

    /*
     * After waiting on the onCompletion() future, users of TransactionCoordinator must call
     * shutdown() to ensure its lifecycle ends cleanly. In particular, the 'scheduler' must be
     * destroyed before its parent scheduler, and calling shutdown() allows the caller to do so
     * at an appropriate time.
     */
    void shutdown();

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
     * Gets a Future that will contain the decision that the coordinator reaches. Note that this
     * will never be signaled unless runCommit has been called.
     */
    SharedSemiFuture<txn::CommitDecision> onDecisionAcknowledged() const;

    /**
     * Returns a future which can be listened on for when all the asynchronous activity spawned by
     * this coordinator has completed. It will always eventually be set and once set it is safe to
     * dispose of the TransactionCoordinator object.
     */
    SharedSemiFuture<void> onCompletion() const;

    /**
     * If runCommit has not yet been called, this will transition this coordinator object to
     * the 'done' state, effectively making it impossible for two-phase commit to be run for this
     * coordinator.
     *
     * Called when a transaction with a higher transaction number is received on this session or
     * when the transaction coordinator service is shutting down.
     */
    void cancelIfCommitNotYetStarted();

    /**
     * Cancels the owned cancellation token which interrupts/cancels all associated
     * `WaitForMajority` invocations under this coordinator. typically invoked only by the
     * TransactionCoordinatorService during stepdown.
     */
    void cancel();

    TxnRetryCounter getTxnRetryCounterForTest() const {
        return *_txnNumberAndRetryCounter.getTxnRetryCounter();
    }

    /**
     * Returns the TransactionCoordinatorMetricsObserver for this TransactionCoordinator.
     */
    const TransactionCoordinatorMetricsObserver& getMetricsObserverForTest() {
        return *_transactionCoordinatorMetricsObserver;
    }

    void reportState(OperationContext* opCtx, BSONObjBuilder& parent) const;
    static std::string toString(Step step);

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
    mutable stdx::mutex _mutex;

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

    // Set when the coordinator has heard back from all the participants and reached a commit
    // decision.
    std::vector<NamespaceString> _affectedNamespaces;

    // Set when the coordinator has durably persisted `_decision` to the `config.coordinators`
    // collection
    bool _decisionDurable{false};
    SharedPromise<txn::CommitDecision> _decisionPromise;

    // Set when the coordinator has received acks from all participants that they have successfully
    // committed or aborted the transaction..
    SharedPromise<txn::CommitDecision> _decisionAcknowledgedPromise;

    // Set when the coordinator has finished all work and the object can be deleted.
    SharedPromise<void> _completionPromise;

    // Store as unique_ptr to avoid a circular dependency between the TransactionCoordinator and
    // the TransactionCoordinatorMetricsObserver.
    std::unique_ptr<TransactionCoordinatorMetricsObserver> _transactionCoordinatorMetricsObserver;

    // The deadline for the TransactionCoordinator to reach a decision
    Date_t _deadline;

    // The cancellation source for WaitForMajority.
    CancellationSource _cancellationSource;
};

}  // namespace mongo
