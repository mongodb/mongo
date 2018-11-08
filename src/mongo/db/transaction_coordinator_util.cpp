
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_util.h"

#include "mongo/db/logical_clock.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/util/functional.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;
using CommitDecision = TransactionCoordinator::CommitDecision;

/**
 * Finds the host and port for a shard.
 */
HostAndPort targetHost(const ShardId& shardId, const ReadPreferenceSetting& readPref) {
    auto opCtx = cc().makeOperationContext();
    auto shardRegistry = Grid::get(opCtx->getServiceContext())->shardRegistry();
    auto swShard = shardRegistry->getShard(opCtx.get(), shardId);
    uassertStatusOK(swShard);
    auto shard = swShard.getValue();
    auto swHostAndPort = shard->getTargeter()->findHostNoWait(readPref);
    uassertStatusOKWithContext(swHostAndPort.getStatus(),
                               str::stream() << "Could not find shard " << shardId);

    return swHostAndPort.getValue();
}

/**
 * Finds the host and port for a shard.
 *
 * Possible errors: [ShardNotFound, ShutdownInProgress]
 *
 * TODO (SERVER-37880): Implement backoff for retries.
 */
Future<HostAndPort> targetHostAsync(ThreadPool* pool,
                                    const ShardId& shardId,
                                    const ReadPreferenceSetting& readPref) {
    return txn::doUntilSuccessOrOneOf({ErrorCodes::ShardNotFound, ErrorCodes::ShutdownInProgress},
                                      [pool, shardId, readPref]() {
                                          return txn::async(pool, [shardId, readPref]() {
                                              return targetHost(shardId, readPref);
                                          });
                                      });
}

/**
 * Sends a command asynchronously to a shard using the given executor. The thread pool is used for
 * targeting the shard.
 *
 * If the command is sent successfully and a response is received, the resulting Future will contain
 * the ResponseStatus from the response.
 *
 * An error status is set on the resulting Future if any of the following occurs:
 *  - If the targeting the shard fails [Possible errors: ShardNotFound, ShutdownInProgress]
 *  - If the command cannot be scheduled to run on the executor [Possible errors:
 *    ShutdownInProgress, maybe others]
 *  - If a response is received that indicates that the destination shard was did not receive the
 *    command or could not process the command (e.g. because the command did not reach the shard due
 *    to a network issue) [Possible errors: Whatever the returned status is in the response]
 */
Future<ResponseStatus> sendAsyncCommandToShard(executor::TaskExecutor* executor,
                                               ThreadPool* pool,
                                               const ShardId& shardId,
                                               const BSONObj& commandObj) {

    auto promiseAndFuture = makePromiseFuture<ResponseStatus>();
    auto sharedPromise =
        std::make_shared<Promise<ResponseStatus>>(std::move(promiseAndFuture.promise));

    auto readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
    targetHostAsync(pool, shardId, readPref)
        .then([ executor, shardId, sharedPromise, commandObj = commandObj.getOwned(), readPref ](
            HostAndPort shardHostAndPort) mutable {

            LOG(3) << "Coordinator going to send command " << commandObj << " to shard " << shardId;

            executor::RemoteCommandRequest request(
                shardHostAndPort, "admin", commandObj, readPref.toContainingBSON(), nullptr);

            auto swCallbackHandle = executor->scheduleRemoteCommand(
                request, [ commandObj = commandObj.getOwned(),
                           shardId,
                           sharedPromise ](const RemoteCommandCallbackArgs& args) mutable {
                    LOG(3) << "Coordinator shard got response " << args.response.data << " for "
                           << commandObj << " to " << shardId;
                    auto status = args.response.status;
                    // Only consider actual failures to send the command as errors.
                    if (status.isOK()) {
                        sharedPromise->emplaceValue(args.response);
                    } else {
                        sharedPromise->setError(status);
                    }
                });

            if (!swCallbackHandle.isOK()) {
                LOG(3) << "Coordinator shard failed to schedule the task to send " << commandObj
                       << " to shard " << shardId << causedBy(swCallbackHandle.getStatus());
                sharedPromise->setError(swCallbackHandle.getStatus());
            }
        })
        .onError([ shardId, commandObj = commandObj.getOwned(), sharedPromise ](Status s) {
            LOG(3) << "Coordinator shard failed to target command " << commandObj << " to shard "
                   << shardId << causedBy(s);

            sharedPromise->setError(s);
        })
        .getAsync([](Status) {});

    // Do not wait for the callback to run. The callback will reschedule the remote request on
    // the same executor if necessary.
    return std::move(promiseAndFuture.future);
}

bool isRetryableError(ErrorCodes::Error code) {
    // TODO (SERVER-37880): Consider using RemoteCommandRetryScheduler.
    return std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                     RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                     code) != RemoteCommandRetryScheduler::kAllRetriableErrors.end() ||
        code == ErrorCodes::NetworkInterfaceExceededTimeLimit;
}

}  // namespace


namespace txn {

// TODO (SERVER-37880): Implement backoff for retries and only retry commands on retryable
// errors.
Future<void> sendDecisionToParticipantShard(executor::TaskExecutor* executor,
                                            ThreadPool* pool,
                                            const ShardId& shardId,
                                            const BSONObj& commandObj) {
    return sendAsyncCommandToShard(executor, pool, shardId, commandObj)
        .then([ executor, pool, shardId, commandObj = commandObj.getOwned() ](
            ResponseStatus response) {

            auto status = getStatusFromCommandResult(response.data);
            auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);
            // There must be no write concern errors in order for us to be able to
            // interpret the command response.
            if (!wcStatus.isOK()) {
                status = wcStatus;
            }

            if (status.isOK()) {
                LOG(3) << "Coordinator shard received OK in response to " << commandObj
                       << "from shard " << shardId;
                return Status::OK();
            } else if (ErrorCodes::isVoteAbortError(status.code())) {
                LOG(3) << "Coordinator shard received a vote abort error in response to "
                       << commandObj << "from shard " << shardId << causedBy(status)
                       << ", interpreting as a successful ack";

                return Status::OK();
            } else {
                // Propagate the error.
                return status;
            }
        })
        .onError(
            [ executor, pool, shardId, commandObj = commandObj.getOwned() ](Status status)
                ->Future<void> {
                    if (isRetryableError(status.code())) {
                        LOG(3) << "Coordinator shard received a retryable error in response to "
                               << commandObj << "from shard " << shardId << causedBy(status)
                               << ", resending command.";

                        return sendDecisionToParticipantShard(executor, pool, shardId, commandObj);
                    } else {
                        LOG(3) << "Coordinator shard received a non-retryable error in response to "
                               << commandObj << "from shard " << shardId << causedBy(status);
                        // Propagate the error.
                        return status;
                    }
                });
}

/**
 * Processes a response from a participant to the prepareTransaction command. An OK response is
 * interpreted as a vote to commit, and the prepareTimestamp is extracted from the response and
 * returned. A response that indicates a vote to abort is interpreted as such, and a retryable error
 * will re-throw so that we can retry the prepare command.
 */
PrepareResponse getCommitVoteFromPrepareResponse(const ShardId& shardId,
                                                 const ResponseStatus& response) {
    uassertStatusOK(getWriteConcernStatusFromCommandResult(response.data));
    auto status = getStatusFromCommandResult(response.data);
    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

    // There must be no write concern errors in order for us to be able to interpret the command
    // response. As an example, it's possible for us to get NoSuchTransaction from a non-primary
    // node of a shard, in which case we might also receive a write concern error but it can't be
    // interpreted as a vote to abort.
    if (!wcStatus.isOK()) {
        status = wcStatus;
    }

    if (status.isOK()) {
        auto prepareTimestampField = response.data["prepareTimestamp"];
        if (prepareTimestampField.eoo() || prepareTimestampField.timestamp().isNull()) {
            LOG(0) << "Coordinator shard received an OK response to prepareTransaction "
                      "without a prepareTimestamp from shard "
                   << shardId << ", which is not expected behavior. Interpreting the response from "
                   << shardId << " as a vote to abort";
            return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
        }

        LOG(3) << "Coordinator shard received a vote to commit from shard " << shardId
               << " with prepareTimestamp: " << prepareTimestampField.timestamp();

        return PrepareResponse{shardId, PrepareVote::kCommit, prepareTimestampField.timestamp()};
    } else if (ErrorCodes::isVoteAbortError(status.code())) {
        LOG(3) << "Coordinator shard received a vote to abort from shard " << shardId;
        return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
    } else if (isRetryableError(status.code())) {
        LOG(3) << "Coordinator shard received a retryable error in response to prepareTransaction "
                  "from shard "
               << shardId << causedBy(status);

        // Rethrow error so that we retry.
        uassertStatusOK(status);
    } else {
        // Non-retryable errors lead to an abort decision.
        LOG(3)
            << "Coordinator shard received a non-retryable error in response to prepareTransaction "
               "from shard "
            << shardId << causedBy(status);
        return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
    }
    MONGO_UNREACHABLE;
}

Future<PrepareResponse> sendPrepareToShard(executor::TaskExecutor* executor,
                                           ThreadPool* pool,
                                           const BSONObj& prepareCommandObj,
                                           const ShardId& shardId,
                                           std::shared_ptr<TransactionCoordinator> coordinator) {

    return sendAsyncCommandToShard(executor, pool, shardId, prepareCommandObj)
        .then([
            coordinator,
            executor,
            pool,
            prepareCommandObj = prepareCommandObj.getOwned(),
            shardId
        ](ResponseStatus response) { return getCommitVoteFromPrepareResponse(shardId, response); })
        .onError<ErrorCodes::ShardNotFound>([shardId](Status s) {
            // ShardNotFound indicates a participant shard was removed, so that is interpreted
            // as an abort decision.
            return Future<PrepareResponse>::makeReady(
                {shardId, CommitDecision::kAbort, boost::none});
        })
        .onError([
            coordinator,
            executor,
            pool,
            prepareCommandObj = prepareCommandObj.getOwned(),
            shardId
        ](Status status) {
            if (isRetryableError(status.code())) {
                if (coordinator->getState() ==
                    TransactionCoordinator::CoordinatorState::kPreparing) {
                    // Retry on error if we're still in the preparing state.
                    return sendPrepareToShard(
                        executor, pool, prepareCommandObj, shardId, coordinator);
                } else {
                    LOG(3) << "Prepare stopped retrying due to concurrent received abort";
                    return Future<PrepareResponse>::makeReady({shardId, boost::none, boost::none});
                }
            } else {
                // Rethrow any non-retryable errors.
                uassertStatusOK(status);
            }
            MONGO_UNREACHABLE;
        });
}

Future<PrepareVoteConsensus> sendPrepare(std::shared_ptr<TransactionCoordinator> coordinator,
                                         executor::TaskExecutor* executor,
                                         ThreadPool* pool,
                                         const std::vector<ShardId>& participantShards,
                                         const LogicalSessionId& lsid,
                                         const TxnNumber& txnNumber) {

    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::InternalMajorityNoSnapshot));

    std::vector<Future<PrepareResponse>> responses;

    // Send prepare to all participants asynchronously and collect their future responses in a
    // vector of responses.
    for (const auto& participant : participantShards) {
        responses.push_back(
            sendPrepareToShard(executor, pool, prepareObj, participant, coordinator));
    }

    // Asynchronously aggregate all prepare responses to find the decision and max prepare timestamp
    // (used for commit), stopping the aggregation as soon as an abort decision is received. Return
    // a future containing the result.
    return collect(
        std::move(responses),
        // Initial value
        PrepareVoteConsensus{boost::none, boost::none},
        // Aggregates an incoming response (next) with the existing aggregate value (result)
        [coordinator](PrepareVoteConsensus& result, const PrepareResponse& next) {
            // There only won't be a vote when a prepareTransaction command fails, and then before
            // retrying it sees that the coordinator is no longer in state 'preparing' due to a
            // concurrently-received vote to abort. This means that a vote to abort must have
            // already been processed by 'collect', and so an unknown vote should never be processed
            // here unless there's a bug in collect.
            invariant(next.vote);
            switch (next.vote.get()) {
                case PrepareVote::kAbort:
                    LOG(3) << "Transaction coordinator received a vote to abort from shard "
                           << next.participantShardId;
                    result.decision = CommitDecision::kAbort;
                    result.maxPrepareTimestamp = boost::none;
                    return ShouldStopIteration::kYes;
                case PrepareVote::kCommit:
                    LOG(3) << "Transaction coordinator received a vote to commit from shard "
                           << next.participantShardId;
                    result.decision = CommitDecision::kCommit;
                    result.maxPrepareTimestamp = (result.maxPrepareTimestamp)
                        ? std::max(result.maxPrepareTimestamp, next.prepareTimestamp)
                        : next.prepareTimestamp;
                    return ShouldStopIteration::kNo;
            }
            MONGO_UNREACHABLE;
        });
}

Future<void> sendCommit(executor::TaskExecutor* executor,
                        ThreadPool* pool,
                        const std::vector<ShardId>& participantShards,
                        const LogicalSessionId& lsid,
                        const TxnNumber& txnNumber,
                        Timestamp commitTimestamp) {
    CommitTransaction commitTransaction;
    commitTransaction.setCommitTimestamp(commitTimestamp);
    commitTransaction.setDbName("admin");
    BSONObj commitObj = commitTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    std::vector<Future<void>> responses;
    for (const auto& participant : participantShards) {
        responses.push_back(sendDecisionToParticipantShard(executor, pool, participant, commitObj));
    }
    return whenAll(responses);
}

Future<void> sendAbort(executor::TaskExecutor* executor,
                       ThreadPool* pool,
                       const std::vector<ShardId>& participantShards,
                       const LogicalSessionId& lsid,
                       const TxnNumber& txnNumber) {
    // TODO (SERVER-36584) Use IDL to create command BSON.
    BSONObj abortObj =
        BSON("abortTransaction" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << txnNumber
                                << "autocommit"
                                << false
                                << WriteConcernOptions::kWriteConcernField
                                << WriteConcernOptions::Majority);

    std::vector<Future<void>> responses;
    for (const auto& participant : participantShards) {
        responses.push_back(sendDecisionToParticipantShard(executor, pool, participant, abortObj));
    }
    return whenAll(responses);
}

void persistParticipantList() {
    // TODO (SERVER-36853): Implement this.
}

CoordinatorCommitDecision persistDecision(PrepareVoteConsensus response) {
    invariant(response.decision);
    CoordinatorCommitDecision coordinatorDecision{response.decision.get(), boost::none};
    LOG(3) << "Coordinator shard persisting commit decision " << response.decision;

    if (response.decision == TransactionCoordinator::CommitDecision::kCommit) {
        invariant(response.maxPrepareTimestamp);
        coordinatorDecision.commitTimestamp = Timestamp(response.maxPrepareTimestamp->getSecs(),
                                                        response.maxPrepareTimestamp->getInc() + 1);
        LOG(3) << "Coordinator shard adjusting cluster time to "
               << coordinatorDecision.commitTimestamp.get();
        Status s = LogicalClock::get(getGlobalServiceContext())
                       ->advanceClusterTime(LogicalTime(response.maxPrepareTimestamp.get()));
        if (!s.isOK()) {
            log() << "Coordinator shard failed to advance cluster time to "
                     "commitTimestamp "
                  << causedBy(s);
        }
    }

    // TODO (SERVER-36853): Actually persist decision.

    return coordinatorDecision;
}

Future<void> whenAll(std::vector<Future<void>>& futures) {
    std::vector<Future<int>> dummyFutures;
    for (auto&& f : futures) {
        dummyFutures.push_back(std::move(f).then([]() { return 0; }));
    }
    return collect(
               std::move(dummyFutures), 0, [](int, const int&) { return ShouldStopIteration::kNo; })
        .ignoreValue();
}
}  // namespace txn

}  // namespace mongo
