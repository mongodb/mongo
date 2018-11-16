
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

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

using Action = TransactionCoordinator::StateMachine::Action;
using State = TransactionCoordinator::StateMachine::State;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;

/**
 * Finds the host and port for a shard.
 */
StatusWith<HostAndPort> targetHost(const ShardId& shardId, const ReadPreferenceSetting& readPref) {
    auto shard = Grid::get(getGlobalServiceContext())->shardRegistry()->getShardNoReload(shardId);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << shardId);
    }

    return shard->getTargeter()->findHostNoWait(readPref);
}

using CallbackFn = stdx::function<void(
    const RemoteCommandCallbackArgs& args, const ShardId& shardId, const BSONObj& commandObj)>;

/**
 * Sends the given command object to the given shard ID. If scheduling and running the command is
 * successful, calls the callback with the status of the command response and the shard ID.
 */
void sendAsyncCommandToShard(executor::TaskExecutor* executor,
                             const ShardId& shardId,
                             const BSONObj& commandObj,
                             const CallbackFn& callbackOnCommandResponse) {
    auto readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
    auto swShardHostAndPort = targetHost(shardId, readPref);
    while (!swShardHostAndPort.isOK()) {
        LOG(3) << "Coordinator shard failed to target primary host of participant shard for "
               << commandObj << causedBy(swShardHostAndPort.getStatus());
        swShardHostAndPort = targetHost(shardId, readPref);
    }

    executor::RemoteCommandRequest request(
        swShardHostAndPort.getValue(), "admin", commandObj, readPref.toContainingBSON(), nullptr);

    auto swCallbackHandle = executor->scheduleRemoteCommand(
        request, [ commandObjOwned = commandObj.getOwned(),
                   shardId,
                   callbackOnCommandResponse ](const RemoteCommandCallbackArgs& args) {
            LOG(3) << "Coordinator shard got response " << args.response.data << " for "
                   << commandObjOwned << " to " << shardId;

            callbackOnCommandResponse(args, shardId, commandObjOwned);
        });

    if (!swCallbackHandle.isOK()) {
        LOG(3) << "Coordinator shard failed to schedule the task to send " << commandObj
               << " to shard " << shardId << causedBy(swCallbackHandle.getStatus());
    }

    // Do not wait for the callback to run. The callback will reschedule the remote request on the
    // same executor if necessary.
}

/**
 * Sends the given command object to all shards in the set of shard IDs. For each shard ID, if
 * scheduling and running the command is successful, calls the callback with the status of the
 * command response and the shard ID.
 */
void sendCommandToShards(OperationContext* opCtx,
                         const std::set<ShardId>& shardIds,
                         const BSONObj& commandObj,
                         const CallbackFn& callbackOnCommandResponse) {
    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    auto exec = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    StringBuilder ss;
    ss << "[";
    for (const auto& shardId : shardIds) {
        sendAsyncCommandToShard(exec, shardId, commandObj, callbackOnCommandResponse);
        ss << shardId << " ";
    }
    ss << "]";
    LOG(3) << "Coordinator shard sending " << commandObj << " to " << ss.str();
}

}  // namespace

namespace txn {

Action sendPrepare(OperationContext* opCtx,
                   const LogicalSessionId& lsid,
                   const TxnNumber& txnNumber,
                   std::shared_ptr<TransactionCoordinator> coordinator,
                   const std::set<ShardId>& participants) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::InternalMajorityNoSnapshot));

    auto actionNotification = std::make_shared<Notification<Action>>();

    CallbackFn prepareCallback;
    prepareCallback = [coordinator, actionNotification, &prepareCallback](
        const RemoteCommandCallbackArgs& args, const ShardId& shardId, const BSONObj& commandObj) {
        if (coordinator->state() != State::kWaitingForVotes) {
            LOG(3)
                << "Coordinator shard not processing prepare response or retrying prepare against "
                << shardId << " because coordinator is no longer waiting for votes";
            return;
        }

        auto status = (!args.response.isOK()) ? args.response.status
                                              : getStatusFromCommandResult(args.response.data);
        if (status.isOK()) {
            status = getWriteConcernStatusFromCommandResult(args.response.data);
        }

        boost::optional<Action> action;

        if (status.isOK()) {
            if (args.response.data["prepareTimestamp"].eoo() ||
                args.response.data["prepareTimestamp"].timestamp().isNull()) {
                LOG(3) << "Coordinator shard received an OK response to prepareTransaction without "
                          "a prepareTimestamp from shard "
                       << shardId
                       << ", which is not expected behavior. Interpreting the response from "
                       << shardId << " as a vote to abort";
                action = coordinator->recvVoteAbort(shardId);
            } else {
                action = coordinator->recvVoteCommit(
                    shardId, args.response.data["prepareTimestamp"].timestamp());
            }
        } else if (ErrorCodes::isVoteAbortError(status.code())) {
            action = coordinator->recvVoteAbort(shardId);
        }

        if (action) {
            if (*action != Action::kNone) {
                actionNotification->set(*action);
            }
            return;
        }

        LOG(3) << "Coordinator shard retrying " << commandObj << " against " << shardId;
        sendAsyncCommandToShard(args.executor, shardId, commandObj, prepareCallback);
    };

    sendCommandToShards(opCtx, participants, prepareObj, prepareCallback);
    return actionNotification->get(opCtx);
}

Action sendCommit(OperationContext* opCtx,
                  const LogicalSessionId& lsid,
                  const TxnNumber& txnNumber,
                  std::shared_ptr<TransactionCoordinator> coordinator,
                  const std::set<ShardId>& nonAckedParticipants,
                  Timestamp commitTimestamp) {
    CommitTransaction commitTransaction;
    commitTransaction.setCommitTimestamp(commitTimestamp);
    commitTransaction.setDbName("admin");
    BSONObj commitObj = commitTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    auto actionNotification = std::make_shared<Notification<Action>>();
    CallbackFn commitCallback;
    commitCallback = [coordinator, actionNotification, &commitCallback](
        const RemoteCommandCallbackArgs& args, const ShardId& shardId, const BSONObj& commandObj) {
        auto status = (!args.response.isOK()) ? args.response.status
                                              : getStatusFromCommandResult(args.response.data);

        if (status.isOK() || ErrorCodes::isVoteAbortError(status.code())) {
            status = getWriteConcernStatusFromCommandResult(args.response.data);
            if (status.isOK()) {
                auto action = coordinator->recvCommitAck(shardId);
                if (action != Action::kNone) {
                    actionNotification->set(action);
                }
                return;
            }
        }

        LOG(3) << "Coordinator shard retrying " << commandObj << " against " << shardId;
        sendAsyncCommandToShard(args.executor, shardId, commandObj, commitCallback);
    };
    sendCommandToShards(opCtx, nonAckedParticipants, commitObj, commitCallback);

    return actionNotification->get(opCtx);
}

Action sendAbort(OperationContext* opCtx,
                 const LogicalSessionId& lsid,
                 const TxnNumber& txnNumber,
                 std::shared_ptr<TransactionCoordinator> coordinator,
                 const std::set<ShardId>& nonVotedAbortParticipants) {
    // TODO (SERVER-36584) Use IDL to create command BSON.
    BSONObj abortObj =
        BSON("abortTransaction" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << txnNumber
                                << "autocommit"
                                << false
                                << WriteConcernOptions::kWriteConcernField
                                << WriteConcernOptions::Majority);

    auto actionNotification = std::make_shared<Notification<Action>>();

    CallbackFn abortCallback;
    abortCallback = [coordinator, actionNotification, &abortCallback](
        const RemoteCommandCallbackArgs& args, const ShardId& shardId, const BSONObj& commandObj) {
        auto status = (!args.response.isOK()) ? args.response.status
                                              : getStatusFromCommandResult(args.response.data);

        if (status.isOK() || ErrorCodes::isVoteAbortError(status.code())) {
            status = getWriteConcernStatusFromCommandResult(args.response.data);
            if (status.isOK()) {
                auto action = coordinator->recvAbortAck(shardId);
                if (action != Action::kNone) {
                    actionNotification->set(action);
                }
                return;
            }
        }

        LOG(3) << "Coordinator shard retrying " << commandObj << " against " << shardId;
        sendAsyncCommandToShard(args.executor, shardId, commandObj, abortCallback);

    };
    sendCommandToShards(opCtx, nonVotedAbortParticipants, abortObj, abortCallback);
    return actionNotification->get(opCtx);
}

}  // namespace txn
}  // namespace mongo
