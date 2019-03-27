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

#include "mongo/db/s/transaction_coordinator_driver.h"
#include "mongo/logger/logstream_builder.h"

namespace mongo {

/**
 * State machine, which implements the two-phase commit protocol for a specific transaction,
 * identified by lsid + txnNumber.
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
     * Instantiates a new TransactioncCoordinator for the specified lsid + txnNumber pair and gives
     * it a 'scheduler' to use for any asynchronous tasks it spawns.
     *
     * If the 'coordinateCommitDeadline' parameter is specified, a timed task will be scheduled to
     * cause the coordinator to be put in a cancelled state, if runCommit is not eventually
     * received.
     */
    TransactionCoordinator(ServiceContext* serviceContext,
                           const LogicalSessionId& lsid,
                           TxnNumber txnNumber,
                           std::unique_ptr<txn::AsyncWorkScheduler> scheduler,
                           boost::optional<Date_t> coordinateCommitDeadline);

    ~TransactionCoordinator();

    /**
     * Represents a decision made by the coordinator, including commit timestamp to be sent with
     * commitTransaction in the case of a decision to commit.
     */
    struct CoordinatorCommitDecision {
        txn::CommitDecision decision;
        boost::optional<Timestamp> commitTimestamp;

        /**
         * Parses a CoordinatorCommitDecision from the object.
         */
        static StatusWith<CoordinatorCommitDecision> fromBSON(const BSONObj& obj);

        /**
         * Returns an instance of CoordinatorCommitDecision from an object.
         *
         * Throws if the object cannot be deserialized.
         */
        static CoordinatorCommitDecision fromBSONThrowing(const BSONObj& obj) {
            return uassertStatusOK(fromBSON(obj));
        };

        /**
         * Returns the BSON representation of this object.
         */
        BSONObj toBSON() const;
    };

    /**
     * The state of the coordinator.
     */
    enum class CoordinatorState {
        // The initial state prior to receiving the participant list from the router.
        kInit,
        // The coordinator is sending prepare and processing responses.
        kPreparing,
        // The coordinator is sending commit messages and waiting for responses.
        kCommitting,
        // The coordinator is sending abort messages and waiting for responses.
        kAborting,
        // The coordinator has received commit/abort acknowledgements from all participants.
        kDone,
    };

    /**
     * The first time this is called, it asynchronously begins the two-phase commit process for the
     * transaction that this coordinator is responsible for.
     *
     * Subsequent calls will not re-run the commit process.
     */
    void runCommit(std::vector<ShardId> participantShards);

    /**
     * To be used to continue coordinating a transaction on step up.
     */
    void continueCommit(const TransactionCoordinatorDocument& doc);

    /**
     * Gets a Future that will contain the decision that the coordinator reaches. Note that this
     * will never be signaled unless runCommit has been called.
     *
     * TODO (SERVER-37364): Remove this when it is no longer needed by the coordinator service.
     */
    SharedSemiFuture<txn::CommitDecision> getDecision();

    /**
     * Returns a future which can be listened on for when all the asynchronous activity spawned by
     * this coordinator has completed. It will always eventually be set and once set it is safe to
     * dispose of the TransactionCoordinator object.
     */
    Future<void> onCompletion();

    /**
     * If runCommit has not yet been called, this will transition this coordinator object to
     * the 'done' state, effectively making it impossible for two-phase commit to be run for this
     * coordinator.
     *
     * Called when a transaction with a higher transaction number is received on this session or
     * when the transaction coordinator service is shutting down.
     */
    void cancelIfCommitNotYetStarted();

private:
    /**
     * Called when the timeout task scheduled by the constructor is no longer necessary (i.e.
     * coordinateCommit was received or the coordinator was cancelled intentionally).
     */
    void _cancelTimeoutWaitForCommitTask();

    /**
     * Expects the participant list to already be majority-committed.
     *
     * 1. Sends prepare and collect the votes (i.e., responses), retrying requests as needed.
     * 2. Based on the votes, makes a commit or abort decision.
     * 3. If the decision is to commit, calculates the commit Timestamp.
     * 4. Writes the decision and waits for the decision to become majority-committed.
     */
    Future<CoordinatorCommitDecision> _runPhaseOne(const std::vector<ShardId>& participantShards);

    /**
     * Expects the decision to already be majority-committed.
     *
     * 1. Send the decision (commit or abort) until receiving all acks (i.e., responses),
     *    retrying requests as needed.
     * 2. Delete the coordinator's durable state without waiting for the delete to become
     *    majority-committed.
     */
    Future<void> _runPhaseTwo(const std::vector<ShardId>& participantShards,
                              const CoordinatorCommitDecision& decision);

    /**
    * Asynchronously sends the commit decision to all participants (commit or abort), resolving the
    * returned future when all participants have acknowledged the decision.
    */
    Future<void> _sendDecisionToParticipants(const std::vector<ShardId>& participantShards,
                                             CoordinatorCommitDecision coordinatorDecision);

    /**
     * Helper for handling errors that occur during either phase of commit coordination.
     */
    void _handleCompletionError(Status s);

    /**
     * Notifies all callers of onCompletion that the commit process has completed by fulfilling
     * their promises, and transitions the state to done.
     *
     * NOTE: Unlocks the lock passed in in order to fulfill the promises.
     *
     * TODO (SERVER-38346): Used SharedSemiFuture to simplify this implementation.
     */
    void _transitionToDone(stdx::unique_lock<stdx::mutex> lk) noexcept;

    // Shortcut to the service context under which this coordinator runs
    ServiceContext* const _serviceContext;

    // The lsid + transaction number that this coordinator is coordinating
    const LogicalSessionId _lsid;
    const TxnNumber _txnNumber;

    // Scheduler to use for all asynchronous activity which needs to be performed by this
    // coordinator
    std::unique_ptr<txn::AsyncWorkScheduler> _scheduler;

    // If not nullptr, references the scheduler containing a task which willl trigger
    // 'cancelIfCommitNotYetStarted' if runCommit has not been invoked. If nullptr, then this
    // coordinator was not created with an expiration task.
    std::unique_ptr<txn::AsyncWorkScheduler> _deadlineScheduler;

    // Context object used to perform and track the state of asynchronous operations on behalf of
    // this coordinator.
    TransactionCoordinatorDriver _driver;

    // Protects the state below
    mutable stdx::mutex _mutex;

    // Stores the current state of the coordinator in the commit process.
    CoordinatorState _state{CoordinatorState::kInit};

    // Promise which will contain the final decision made by the coordinator (whether to commit or
    // abort). This is only known once all responses to prepare have been received from all
    // participants, and the collective decision has been majority persisted to
    // config.transactionCommitDecisions.
    SharedPromise<txn::CommitDecision> _decisionPromise;

    // A list of all promises corresponding to futures that were returned to callers of
    // onCompletion.
    //
    // TODO (SERVER-38346): Remove this when SharedSemiFuture supports continuations.
    std::vector<Promise<void>> _completionPromises;
};

logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream,
                                     const TransactionCoordinator::CoordinatorState& state);

logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream,
                                     const txn::CommitDecision& decision);

}  // namespace mongo
