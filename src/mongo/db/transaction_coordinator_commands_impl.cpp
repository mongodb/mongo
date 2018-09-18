/**
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_commands_impl.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

/**
 * Finds the host and port for a shard.
 */
StatusWith<HostAndPort> targetHost(OperationContext* opCtx,
                                   const ShardId& shardId,
                                   const ReadPreferenceSetting& readPref) {
    auto shard = Grid::get(getGlobalServiceContext())->shardRegistry()->getShardNoReload(shardId);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << shardId);
    }

    auto targeter = shard->getTargeter();
    return targeter->findHost(opCtx, readPref);
}

using CallbackFn = stdx::function<void(Status status, const ShardId& shardID)>;

/**
 * Sends the given command object to the given shard ID. If scheduling and running the command is
 * successful, calls the callback with the status of the command response and the shard ID.
 */
void sendAsyncCommandToShard(StringData commandName,
                             OperationContext* opCtx,
                             executor::TaskExecutor* executor,
                             const ShardId& shardId,
                             const BSONObj& commandObj,
                             CallbackFn callbackOnCommandResponse) {
    auto readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
    auto swShardHostAndPort = targetHost(opCtx, shardId, readPref);
    if (!swShardHostAndPort.isOK()) {
        LOG(0) << "Targeting shard for " << commandName << " failed"
               << causedBy(swShardHostAndPort.getStatus());
        return;
    }

    executor::RemoteCommandRequest request(
        swShardHostAndPort.getValue(), "admin", commandObj, readPref.toContainingBSON(), nullptr);

    auto scheduleRemoteCommandStatus = executor->scheduleRemoteCommand(
        request,
        [commandName, shardId, callbackOnCommandResponse](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {

            auto status = (!args.response.isOK()) ? args.response.status
                                                  : getStatusFromCommandResult(args.response.data);

            // TODO (SERVER-36687): Remove log line or demote to lower log level
            // once cross-shard transactions are stable.
            LOG(0) << "Coordinator shard got response " << status << " for " << commandName
                   << " to " << shardId;

            // Only call callback if command successfully executed and got a response.
            if (args.response.isOK()) {
                callbackOnCommandResponse(status, shardId);
            }
        });

    if (!scheduleRemoteCommandStatus.isOK()) {
        LOG(0) << "Coordinator shard failed to schedule the task to send " << commandName
               << " to shard " << shardId;
    }
}

/**
 * Sends the given command object to all shards in the set of shard IDs. For each shard ID, if
 * scheduling and running the command is successful, calls the callback with the status of the
 * command response and the shard ID.
 */
void sendAsyncCommandToShards(StringData commandName,
                              OperationContext* opCtx,
                              const std::set<ShardId>& shardIds,
                              const BSONObj& commandObj,
                              CallbackFn callbackOnCommandResponse) {
    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    auto exec = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    StringBuilder ss;
    ss << "[";
    // For each non-acked participant, launch an async task to target its shard
    // and then asynchronously send the command.
    for (const auto& shardId : shardIds) {
        sendAsyncCommandToShard(
            commandName, opCtx, exec, shardId, commandObj, callbackOnCommandResponse);
        ss << shardId << " ";
    }

    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    ss << "]";
    LOG(0) << "Coordinator shard sending " << commandObj << " to " << ss.str();
}

void sendCommit(OperationContext* opCtx,
                std::shared_ptr<TransactionCoordinator> coordinator,
                const std::set<ShardId>& nonAckedParticipants,
                Timestamp commitTimestamp) {
    invariant(coordinator);

    CommitTransaction commitTransaction;
    commitTransaction.setCommitTimestamp(commitTimestamp);
    commitTransaction.setDbName("admin");
    BSONObj commitObj = commitTransaction.toBSON(BSON(
        "lsid" << opCtx->getLogicalSessionId()->toBSON() << "txnNumber" << *opCtx->getTxnNumber()
               << "autocommit"
               << false));

    sendAsyncCommandToShards(CommitTransaction::kCommandName,
                             opCtx,
                             nonAckedParticipants,
                             commitObj,
                             [coordinator](Status commandResponseStatus, const ShardId& shardId) {
                                 // TODO (SERVER-36642): Also interpret TransactionTooOld as
                                 // acknowledgment.
                                 if (commandResponseStatus.isOK()) {
                                     coordinator->recvCommitAck(shardId);
                                 }
                             });
}

void sendAbort(OperationContext* opCtx, const std::set<ShardId>& nonVotedAbortParticipants) {
    // TODO (SERVER-36584) Use IDL to create command BSON.
    BSONObj abortObj = BSON(
        "abortTransaction" << 1 << "lsid" << opCtx->getLogicalSessionId()->toBSON() << "txnNumber"
                           << *opCtx->getTxnNumber()
                           << "autocommit"
                           << false);

    sendAsyncCommandToShards(
        "abortTransaction", opCtx, nonVotedAbortParticipants, abortObj, [](Status, const ShardId&) {
        });
}

void doAction(OperationContext* opCtx,
              std::shared_ptr<TransactionCoordinator> coordinator,
              TransactionCoordinator::StateMachine::Action action) {
    switch (action) {
        case TransactionCoordinator::StateMachine::Action::kSendCommit: {
            sendCommit(opCtx,
                       coordinator,
                       coordinator->getNonAckedCommitParticipants(),
                       coordinator->getCommitTimestamp());
            return;
        }
        case TransactionCoordinator::StateMachine::Action::kSendAbort: {
            sendAbort(opCtx, coordinator->getNonVotedAbortParticipants());
            return;
        }
        case TransactionCoordinator::StateMachine::Action::kNone:
            return;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

namespace txn {

void recvCoordinateCommit(OperationContext* opCtx,
                          std::shared_ptr<TransactionCoordinator> coordinator,
                          const std::set<ShardId>& participantList) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    StringBuilder ss;
    ss << "[";
    for (const auto& shardId : participantList) {
        ss << shardId << " ";
    }
    ss << "]";
    LOG(0) << "Coordinator shard received participant list with shards " << ss.str();

    TransactionCoordinator::StateMachine::Action action;
    action = coordinator->recvCoordinateCommit(participantList);
    doAction(opCtx, coordinator, action);

    // TODO (SERVER-36640): Wait for decision to be made.
}

void recvVoteCommit(OperationContext* opCtx,
                    std::shared_ptr<TransactionCoordinator> coordinator,
                    const ShardId& shardId,
                    Timestamp prepareTimestamp) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    LOG(0) << "Coordinator shard received voteCommit from " << shardId << " with prepare timestamp "
           << prepareTimestamp;

    TransactionCoordinator::StateMachine::Action action;
    action = coordinator->recvVoteCommit(shardId, prepareTimestamp);
    doAction(opCtx, coordinator, action);
}

void recvVoteAbort(OperationContext* opCtx,
                   std::shared_ptr<TransactionCoordinator> coordinator,
                   const ShardId& shardId) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    LOG(0) << "Coordinator shard received voteAbort from " << shardId;

    TransactionCoordinator::StateMachine::Action action;
    action = coordinator->recvVoteAbort(shardId);
    doAction(opCtx, coordinator, action);
}

}  // namespace txn
}  // namespace mongo
