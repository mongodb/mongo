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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_commands_impl.h"

#include "mongo/db/session_catalog.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

std::vector<ShardId> sendCommit(OperationContext* opCtx, std::set<ShardId>& nonAckedParticipants) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : nonAckedParticipants) {
        // TODO (SERVER-36584): Use the commitTransaction IDL to create the command BSON.
        requests.emplace_back(shardId, BSON("commitTransaction" << 1));
    }

    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                            "admin",
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

    std::vector<ShardId> ackedParticipants;
    while (!ars.done()) {
        auto response = ars.next();
        // TODO (SERVER-36642): Also interpret TransactionTooOld as acknowledgment.
        if (response.swResponse.getStatus().isOK() &&
            getStatusFromCommandResult(response.swResponse.getValue().data).isOK()) {
            ackedParticipants.push_back(response.shardId);
        }
    }
    return ackedParticipants;
}

std::vector<ShardId> sendAbort(OperationContext* opCtx, std::set<ShardId>& nonAckedParticipants) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : nonAckedParticipants) {
        // TODO (SERVER-36584) Use IDL to create command BSON.
        requests.emplace_back(shardId, BSON("abortTransaction" << 1));
    }

    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                            "admin",
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

    // TODO (SERVER-36638): The ARS does not currently support "fire-and-forget" messages; the ARS
    // uses the caller's thread to send the messages over the network inside calls to next().
    std::vector<ShardId> ackedParticipants;
    while (!ars.done()) {
        auto response = ars.next();
        // TODO (SERVER-36642): Also interpret NoSuchTransaction and TransactionTooOld as
        // acknowledgment.
        if (response.swResponse.getStatus().isOK() &&
            getStatusFromCommandResult(response.swResponse.getValue().data).isOK()) {
            ackedParticipants.push_back(response.shardId);
        }
    }
    return ackedParticipants;
}

void doAction(OperationContext* opCtx,
              TransactionCoordinator& coordinator,
              TransactionCoordinator::StateMachine::Action action) {
    switch (action) {
        case TransactionCoordinator::StateMachine::Action::kSendCommit: {
            auto nonAckedParticipants = coordinator.getNonAckedCommitParticipants();

            // TODO (SERVER-36638): Spawn a separate thread to do this so that the client's thread
            // does not block.

            // TODO (SERVER-36645) Check the Session back in.
            auto ackedParticipants = sendCommit(opCtx, nonAckedParticipants);

            // TODO (SERVER-36645) Check the Session back out.
            for (auto& participant : ackedParticipants) {
                coordinator.recvCommitAck(participant);
            }

            return;
        }
        case TransactionCoordinator::StateMachine::Action::kSendAbort: {
            auto nonAckedParticipants = coordinator.getNonAckedAbortParticipants();

            // TODO (SERVER-36638): Spawn a separate thread to do this so that the client's thread
            // does not block.

            // TODO (SERVER-36645) Check the Session back in.
            auto ackedParticipants = sendAbort(opCtx, nonAckedParticipants);

            // TODO (SERVER-36645) Check the Session back out.
            for (auto& participant : ackedParticipants) {
                coordinator.recvAbortAck(participant);
            }

            return;
        }
        case TransactionCoordinator::StateMachine::Action::kNone:
            return;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

namespace txn {

void recvCoordinateCommit(OperationContext* opCtx, const std::set<ShardId>& participantList) {
    auto& coordinator = TransactionCoordinator::get(opCtx);
    auto action = coordinator->recvCoordinateCommit(participantList);
    doAction(opCtx, *coordinator, action);

    // TODO (SERVER-36640): Wait for decision to be made.
}

void recvVoteCommit(OperationContext* opCtx, const ShardId& shardId, int prepareTimestamp) {
    auto& coordinator = TransactionCoordinator::get(opCtx);
    auto action = coordinator->recvVoteCommit(shardId, prepareTimestamp);
    doAction(opCtx, *coordinator, action);
}

void recvVoteAbort(OperationContext* opCtx, const ShardId& shardId) {
    auto& coordinator = TransactionCoordinator::get(opCtx);
    auto action = coordinator->recvVoteAbort(shardId);
    doAction(opCtx, *coordinator, action);
}

}  // namespace txn
}  // namespace mongo
