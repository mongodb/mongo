
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

#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/transaction_coordinator.h"
#include "mongo/db/transaction_coordinator_document_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <vector>

namespace mongo {

namespace txn {

/**
 * An alias to indicate a vote from a participant. This just makes it clearer what's going on in
 * different stages of the commit process.
 */
using PrepareVote = TransactionCoordinator::CommitDecision;

/**
 * Represents a response to prepareTransaction from a single participant. The timestamp will only be
 * present if the participant votes to commit (indicated by the decision field).
 */
struct PrepareResponse {
    ShardId participantShardId;
    boost::optional<PrepareVote> vote;
    boost::optional<Timestamp> prepareTimestamp;
};

/**
 * Represents the aggregate of all prepare responses, including the decision that should be made and
 * the max of all prepare timestamps received in the case of a decision to commit.
 */
struct PrepareVoteConsensus {
    boost::optional<TransactionCoordinator::CommitDecision> decision;
    boost::optional<Timestamp> maxPrepareTimestamp;
};

/**
 * Represents a decision made by the coordinator, including commit timestamp to be sent with
 * commitTransaction in the case of a decision to commit.
 */
struct CoordinatorCommitDecision {
    TransactionCoordinator::CommitDecision decision;
    boost::optional<Timestamp> commitTimestamp;
};

/**
 * Sends a command corresponding to a commit decision (i.e. commitTransaction or abortTransaction)
 * to the given shard ID and retries on any retryable error until the command succeeds or receives a
 * response that may be interpreted as a vote to abort (e.g.  NoSuchTransaction). Used for
 * sendCommit and sendAbort.
 */
Future<void> sendDecisionToParticipantShard(executor::TaskExecutor* executor,
                                            ThreadPool* pool,
                                            const ShardId& shardId,
                                            const BSONObj& commandObj);

/**
 * Sends prepare to a given shard, retrying until a response is received from the participant or
 * until the coordinator is no longer in a preparing state due to having already received a vote to
 * abort from another participant. Returns a future containing the participant's response
 */
Future<PrepareResponse> sendPrepareToShard(executor::TaskExecutor* executor,
                                           ThreadPool* pool,
                                           const BSONObj& prepareCommandObj,
                                           const ShardId& shardId,
                                           std::shared_ptr<TransactionCoordinator> coordinator);

/**
 * Sends prepare to all participants and returns a future that will be resolved when either:
 *    a) All participants have responded with a vote to commit, or
 *    b) At least one participant votes to abort.
 *
 * If all participants vote to commit, the result will contain the max prepare timestamp of all
 * prepare timestamps attached to the participants' responses. Otherwise the result will simply
 * contain the decision to abort.
 */
Future<PrepareVoteConsensus> sendPrepare(std::shared_ptr<TransactionCoordinator> coordinator,
                                         executor::TaskExecutor* executor,
                                         ThreadPool* pool,
                                         const std::vector<ShardId>& participantShards,
                                         const LogicalSessionId& lsid,
                                         const TxnNumber& txnNumber);

/**
 * Sends commit to all shards and returns a future that will be resolved when all participants have
 * responded with success.
 */
Future<void> sendCommit(executor::TaskExecutor* executor,
                        ThreadPool* pool,
                        const std::vector<ShardId>& participantShards,
                        const LogicalSessionId& lsid,
                        const TxnNumber& txnNumber,
                        Timestamp commitTimestamp);

/**
 * Sends abort to all shards and returns a future that will be resolved when all participants have
 * responded with success.
 */
Future<void> sendAbort(executor::TaskExecutor* executor,
                       ThreadPool* pool,
                       const std::vector<ShardId>& participantShards,
                       const LogicalSessionId& lsid,
                       const TxnNumber& txnNumber);

/**
 * Upserts a document of the form:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
 *    participants: ["shard0000", "shard0001"]
 * }
 *
 * into config.transaction_coordinators and waits for the upsert to be majority-committed.
 *
 * Throws if the upsert fails or waiting for writeConcern fails.
 * If the upsert returns a DuplicateKey error, converts it to an anonymous error, because it means
 * a document for the (lsid, txnNumber) exists with a different participant list.
 */
void persistParticipantList(OperationContext* opCtx,
                            LogicalSessionId lsid,
                            TxnNumber txnNumber,
                            const std::vector<ShardId>& participantList);

/**
 * If 'commitTimestamp' is boost::none, updates the document in config.transaction_coordinators for
 * (lsid, txnNumber) to be:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
 *    participants: ["shard0000", "shard0001"]
 *    decision: "abort"
 * }
 *
 * else updates the document to be:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
 *    participants: ["shard0000", "shard0001"]
 *    decision: "commit"
 *    commitTimestamp: Timestamp(xxxxxxxx, x),
 * }
 *
 * and waits for the update to be majority-committed.
 *
 * Throws if the update fails or waiting for writeConcern fails.
 * If the update succeeds but did not update any document, throws an anonymous error, because it
 * means either no document for (lsid, txnNumber) exists, or a document exists but has a different
 * participant list, different decision, or different commit Timestamp.
 */
void persistDecision(OperationContext* opCtx,
                     LogicalSessionId lsid,
                     TxnNumber txnNumber,
                     const std::vector<ShardId>& participantList,
                     const boost::optional<Timestamp>& commitTimestamp);

/**
 * Deletes the document in config.transaction_coordinators for (lsid, txnNumber).
 *
 * Does *not* wait for the delete to be majority-committed.
 *
 * Throws if the update fails.
 * If the update succeeds but did not update any document, throws an anonymous error, because it
 * means either no document for (lsid, txnNumber) exists, or a document exists but without a
 * decision.
 */
void deleteCoordinatorDoc(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber);

/**
 * Reads and returns all documents in config.transaction_coordinators.
 */
std::vector<TransactionCoordinatorDocument> readAllCoordinatorDocs(OperationContext* opCtx);

//
// BELOW THIS ARE FUTURES-RELATED UTILITIES.
//

enum class ShouldStopIteration { kYes, kNo };

/**
 * Helper function that allows you to asynchronously aggregate the results of a vector of Futures.
 * It's essentially an async foldLeft, with the ability to decide to stop processing results before
 * they have all come back. The combiner function specifies how to take an incoming result (the
 * second parameter) and combine it to create the final ('global') result (the first parameter). The
 * inital value for the 'global result' is specified by initValue.
 *
 * Example from the unit tests:
 *
 *TEST_F(TransactionCoordinatorTest, CollectReturnsCombinedResultWithSeveralInputFutures) {
 *
 *     std::vector<Future<int>> futures;
 *     std::vector<Promise<int>> promises;
 *     std::vector<int> futureValues;
 *     for (int i = 0; i < 5; ++i) {
 *         auto pf = makePromiseFuture<int>();
 *         futures.push_back(std::move(pf.future));
 *         promises.push_back(std::move(pf.promise));
 *         futureValues.push_back(i);
 *     }
 *
 *     // Sum all of the inputs.
 *     auto resultFuture = collect<int, int>(futures, 0, [](int& result, const int& next) {
 *         result += next;
 *         return true;
 *     });
 *
 *     for (size_t i = 0; i < promises.size(); ++i) {
 *         promises[i].emplaceValue(futureValues[i]);
 *     }
 *
 *     // Result should be the sum of all the values emplaced into the promises.
 *     ASSERT_EQ(resultFuture.get(), std::accumulate(futureValues.begin(), futureValues.end(), 0));
 * }
 *
 */
template <class IndividualResult, class GlobalResult, class Callable>
Future<GlobalResult> collect(std::vector<Future<IndividualResult>>&& futures,
                             GlobalResult&& initValue,
                             Callable&& combiner) {
    if (futures.size() == 0) {
        return initValue;
    }

    /**
     * Shared state for the continuations of the individual futures in the array.
     */
    struct SharedBlock {
        SharedBlock(size_t numOutstandingResponses,
                    GlobalResult globalResult,
                    Promise<GlobalResult> resultPromise,
                    Callable&& combiner)
            : numOutstandingResponses(numOutstandingResponses),
              globalResult(std::move(globalResult)),
              resultPromise(std::move(resultPromise)),
              combiner(std::move(combiner)) {}
        /*****************************************************
         * The first few fields have fixed values.           *
        ******************************************************/
        // Protects all state in the SharedBlock.
        stdx::mutex mutex;
        // Whether or not collect has finished collecting responses.
        bool done{false};

        /*****************************************************
         * The below have initial values based on user input.*
        ******************************************************/
        // The number of input futures that have not yet been resolved and processed.
        size_t numOutstandingResponses;
        // The variable where the intermediate results and final result
        // is stored.
        GlobalResult globalResult;
        // The promise to be fulfilled when the result is ready.
        Promise<GlobalResult> resultPromise;
        // The input combiner function.
        Callable combiner;
    };

    // Create the promise and future used to fulfill the result.
    auto resultPromiseAndFuture = makePromiseFuture<GlobalResult>();

    // Create the shared context used by all continuations
    auto sharedBlock = std::make_shared<SharedBlock>(futures.size(),
                                                     std::move(initValue),
                                                     std::move(resultPromiseAndFuture.promise),
                                                     std::move(combiner));

    // For every input future, add a continuation that will asynchronously update the
    // SharedBlock upon completion of the input future.
    for (auto&& localFut : futures) {
        std::move(localFut)
            // If the input future is successful, increment the number of resolved futures and apply
            // the combiner to the new input.
            .then([sharedBlock](IndividualResult res) {
                stdx::unique_lock<stdx::mutex> lk(sharedBlock->mutex);
                if (!sharedBlock->done) {
                    sharedBlock->numOutstandingResponses--;

                    // Process responses until the combiner function returns false or all inputs
                    // have been resolved.
                    bool shouldStopProcessingResponses =
                        sharedBlock->combiner(sharedBlock->globalResult, std::move(res)) ==
                        ShouldStopIteration::kYes;

                    if (sharedBlock->numOutstandingResponses == 0 ||
                        shouldStopProcessingResponses) {
                        sharedBlock->done = true;
                        // Unlock before emplacing the result in case any continuations do expensive
                        // work.
                        lk.unlock();
                        sharedBlock->resultPromise.emplaceValue(sharedBlock->globalResult);
                    }
                }
            })
            // If the input future completes with an error, also set an error on the output promise
            // and stop processing responses.
            .onError([sharedBlock](Status s) {
                stdx::unique_lock<stdx::mutex> lk(sharedBlock->mutex);
                if (!sharedBlock->done) {
                    sharedBlock->done = true;
                    // Unlock before emplacing the result in case any continuations do expensive
                    // work.
                    lk.unlock();
                    sharedBlock->resultPromise.setError(s);
                }
            })
            // Asynchronously execute the above call chain rather than wait for a response.
            .getAsync([](Status s) {});
    }

    return std::move(resultPromiseAndFuture.future);
}

/**
 * A thin wrapper around ThreadPool::schedule that returns a future that will be resolved on the
 * completion of the task or rejected if the task errors.
 */
template <class Callable>
Future<FutureContinuationResult<Callable>> async(ThreadPool* pool, Callable&& task) {
    using ReturnType = decltype(task());
    auto pf = makePromiseFuture<ReturnType>();
    auto taskCompletionPromise = std::make_shared<Promise<ReturnType>>(std::move(pf.promise));
    auto scheduleStatus = pool->schedule(
        [ task = std::forward<Callable>(task), taskCompletionPromise ]() mutable noexcept {
            taskCompletionPromise->setWith(task);
        });

    if (!scheduleStatus.isOK()) {
        taskCompletionPromise->setError(scheduleStatus);
    }

    return std::move(pf.future);
}

/**
 * Returns a future that will be resolved when all of the input futures have resolved, or rejected
 * when any of the futures is rejected.
 */
Future<void> whenAll(std::vector<Future<void>>& futures);


/**
 * Executes a function returning a Future until the function does not return an error status or
 * until one of the provided error codes is returned.
 *
 * TODO (SERVER-37880): Implement backoff for retries.
 */
template <class Callable>
Future<FutureContinuationResult<Callable>> doUntilSuccessOrOneOf(
    std::set<ErrorCodes::Error>&& errorsToHaltOn, Callable&& f) {
    auto future = f();
    return std::move(future).onError(
        [ errorsToHaltOn = std::move(errorsToHaltOn),
          f = std::forward<Callable>(f) ](Status s) mutable {
            // If this error is one of the errors we should halt on, rethrow the error and don't
            // retry.
            if (errorsToHaltOn.find(s.code()) != errorsToHaltOn.end()) {
                uassertStatusOK(s);
            }
            return doUntilSuccessOrOneOf(std::move(errorsToHaltOn), std::forward<Callable>(f));
        });
}

}  // namespace txn
}  // namespace mongo
