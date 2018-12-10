
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

#include "mongo/db/logical_session_id.h"
#include "mongo/db/transaction_coordinator_document_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

namespace txn {
struct PrepareResponse;
struct PrepareVoteConsensus;
struct CoordinatorCommitDecision;
}

/**
 * Class responsible for coordinating two-phase commit across shards.
 */
class TransactionCoordinator : public std::enable_shared_from_this<TransactionCoordinator> {
public:
    TransactionCoordinator(executor::TaskExecutor* networkExecutor,
                           ThreadPool* callbackPool,
                           const LogicalSessionId& lsid,
                           const TxnNumber& txnNumber)
        : _networkExecutor(networkExecutor),
          _callbackPool(callbackPool),
          _lsid(lsid),
          _txnNumber(txnNumber),
          _state(CoordinatorState::kInit) {}

    ~TransactionCoordinator();

    /**
     * The decision made by the coordinator whether to commit or abort the transaction.
     */
    enum class CommitDecision {
        kCommit,
        kAbort,
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
     * transaction that this coordinator is responsible for, and returns a future that will be
     * resolved when a commit or abort decision has been made and persisted.
     *
     * Subsequent calls will not re-run the commit process, but instead return a future that will be
     * resolved when the original commit process that was kicked off comes to a decision. If the
     * original commit process has completed, returns a ready future containing the final decision.
     */
    SharedSemiFuture<CommitDecision> runCommit(const std::vector<ShardId>& participantShards);

    /**
     * To be used to continue coordinating a transaction on step up.
     */
    void continueCommit(const TransactionCoordinatorDocument& doc);

    /**
     * Returns a future that will be signaled when the transaction has completely finished
     * committing or aborting (i.e. when commit/abort acknowledgements have been received from all
     * participants, or the coordinator commit process is aborted locally for some reason).
     *
     * Unlike runCommit, this will not kick off the commit process if it has not already begun.
     */
    Future<void> onCompletion();

    /**
     * Gets a Future that will contain the decision that the coordinator reaches.
     *
     * TODO (SERVER-37364): Remove this when it is no longer needed by the coordinator service.
     */
    SharedSemiFuture<CommitDecision> getDecision() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _finalDecisionPromise.getFuture();
    }

    /**
     * If runCommit has not yet been called, this will transition this coordinator object to
     * the 'done' state, effectively making it impossible for two-phase commit to be run for this
     * coordinator.
     *
     * Called when a transaction with a higher transaction number is received on this session.
     */
    void cancelIfCommitNotYetStarted();

    /**
     * Gets the current state of the coordinator.
     *
     * TODO (SERVER-38345): Consider making this state private.
     */
    CoordinatorState getState() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _state;
    }

    // NOTE: SHOULD BE USED IN TESTS ONLY.
    void setState_forTest(CoordinatorState state) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _state = state;
    }

private:
    /**
     * Expects the participant list to already be majority-committed.
     *
     * 1. Sends prepare and collect the votes (i.e., responses), retrying requests as needed.
     * 2. Based on the votes, makes a commit or abort decision.
     * 3. If the decision is to commit, calculates the commit Timestamp.
     * 4. Writes the decision and waits for the decision to become majority-committed.
     */
    Future<txn::CoordinatorCommitDecision> _runPhaseOne(
        const std::vector<ShardId>& participantShards);

    /**
     * Expects the decision to already be majority-committed.
     *
     * 1. Send the decision (commit or abort) until receiving all acks (i.e., responses),
     *    retrying requests as needed.
     * 2. Delete the coordinator's durable state without waiting for the delete to become
     *    majority-committed.
     */
    Future<void> _runPhaseTwo(const std::vector<ShardId>& participantShards,
                              const txn::CoordinatorCommitDecision& decision);

    /**
    * Asynchronously sends the commit decision to all participants (commit or abort), resolving the
    * returned future when all participants have acknowledged the decision.
    */
    Future<void> _sendDecisionToParticipants(const std::vector<ShardId>& participantShards,
                                             txn::CoordinatorCommitDecision coordinatorDecision);

    /**
     * Helper for handling errors that occur during either phase of commit coordination.
     */
    void _handleCompletionStatus(Status s);

    /**
     * Notifies all callers of onCompletion that the commit process has completed by fulfilling
     * their promises, and transitions the state to done.
     *
     * NOTE: Unlocks the lock passed in in order to fulfill the promises.
     *
     * TODO (SERVER-38346): Used SharedSemiFuture to simplify this implementation.
     */
    void _transitionToDone(stdx::unique_lock<stdx::mutex> lk) noexcept;

    /**
     * A task executor used to execute all network requests used to send messages to participants.
     * The only current networking that may occur outside of this is when targeting a shard to find
     * its host and port.
     *
     * Note: Memory/object not owned by the coordinator.
     */
    executor::TaskExecutor* _networkExecutor;

    /**
     * A thread pool used to execute any code that should be non-blocking, e.g. persisting the
     * participant list or the commit decision to disk.
     *
     * Note: Memory/object not owned by the coordinator.
     */
    ThreadPool* _callbackPool;

    /**
     * The logical session id of the transaction that this coordinator is coordinating.
     */
    LogicalSessionId _lsid;

    /**
     * The transaction number of the transaction that this coordinator is coordinating.
     */
    TxnNumber _txnNumber;

    /**
     * Protects _state, _finalDecisionPromise, and _completionPromises.
     */
    stdx::mutex _mutex;

    /**
     * The current state of the coordinator in the commit process.
     */
    CoordinatorState _state;

    /**
     * A promise that will contain the final decision made by the coordinator (whether to commit or
     * abort). This is only known once all responses to prepare have been received from all
     * participants, and the collective decision has been persisted to
     * config.transactionCommitDecisions.
     */
    SharedPromise<CommitDecision> _finalDecisionPromise;

    /**
     * A list of all promises corresponding to futures that were returned to callers of
     * onCompletion.
     *
     * TODO (SERVER-38346): Remove this when SharedSemiFuture supports continuations.
     */
    std::vector<Promise<void>> _completionPromises;
};

inline logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream,
                                            const TransactionCoordinator::CoordinatorState& state) {
    using State = TransactionCoordinator::CoordinatorState;
    // clang-format off
    switch (state) {
        case State::kInit:  stream.stream() << "kInit"; break;
        case State::kPreparing:   stream.stream() << "kPreparing"; break;
        case State::kAborting: stream.stream() << "kAborting"; break;
        case State::kCommitting: stream.stream() << "kCommitting"; break;
        case State::kDone: stream.stream() << "kDone"; break;
    };
    // clang-format on
    return stream;
}

inline logger::LogstreamBuilder& operator<<(
    logger::LogstreamBuilder& stream, const TransactionCoordinator::CommitDecision& decision) {
    using Decision = TransactionCoordinator::CommitDecision;
    // clang-format off
    switch (decision) {
        case Decision::kCommit:  stream.stream() << "kCommit"; break;
        case Decision::kAbort:   stream.stream() << "kAbort"; break;
    };
    // clang-format on
    return stream;
}
}
