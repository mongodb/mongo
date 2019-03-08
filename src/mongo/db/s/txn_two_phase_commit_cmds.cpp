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

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic);

class PrepareTransactionCmd : public TypedCommand<PrepareTransactionCmd> {
public:
    class PrepareTimestamp {
    public:
        PrepareTimestamp(Timestamp timestamp) : _timestamp(std::move(timestamp)) {}
        void serialize(BSONObjBuilder* bob) const {
            bob->append("prepareTimestamp", _timestamp);
        }

    private:
        Timestamp _timestamp;
    };

    using Request = PrepareTransaction;
    using Response = PrepareTimestamp;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            if (!getTestCommandsEnabled() &&
                serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            // We automatically fail 'prepareTransaction' against a primary that has
            // 'enableMajorityReadConcern' set to 'false'.
            uassert(50993,
                    "'prepareTransaction' is not supported with 'enableMajorityReadConcern=false'",
                    serverGlobalParams.enableMajorityReadConcern);

            // We do not allow preparing a transaction if the replica set has any arbiters.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(50995,
                    "'prepareTransaction' is not supported for replica sets with arbiters",
                    !replCoord->setContainsArbiter());

            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "prepareTransaction must be run within a transaction",
                    txnParticipant);

            LOG(3)
                << "Participant shard received prepareTransaction for transaction with txnNumber "
                << opCtx->getTxnNumber() << " on session "
                << opCtx->getLogicalSessionId()->toBSON();

            uassert(ErrorCodes::CommandNotSupported,
                    "'prepareTransaction' is only supported in feature compatibility version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction isn't in progress",
                    txnParticipant.inMultiDocumentTransaction());

            if (txnParticipant.transactionIsPrepared()) {
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                auto prepareOpTime = txnParticipant.getPrepareOpTime();

                // Set the client optime to be prepareOpTime if it's not already later than
                // prepareOpTime. This ensures that we wait for writeConcern and that prepareOpTime
                // will be committed.
                if (prepareOpTime > replClient.getLastOp()) {
                    replClient.setLastOp(prepareOpTime);
                }

                invariant(opCtx->recoveryUnit()->getPrepareTimestamp() ==
                              prepareOpTime.getTimestamp(),
                          str::stream() << "recovery unit prepareTimestamp: "
                                        << opCtx->recoveryUnit()->getPrepareTimestamp().toString()
                                        << " participant prepareOpTime: "
                                        << prepareOpTime.toString());

                if (MONGO_FAIL_POINT(
                        participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic)) {
                    uasserted(ErrorCodes::HostUnreachable,
                              "returning network error because failpoint is on");
                }
                return PrepareTimestamp(prepareOpTime.getTimestamp());
            }

            const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx, {});
            if (MONGO_FAIL_POINT(
                    participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic)) {
                uasserted(ErrorCodes::HostUnreachable,
                          "returning network error because failpoint is on");
            }
            return PrepareTimestamp(std::move(prepareTimestamp));
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Prepares a transaction on this shard; sent by a router or re-sent by the "
               "transaction commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

} prepareTransactionCmd;

class CoordinateCommitTransactionCmd : public TypedCommand<CoordinateCommitTransactionCmd> {
public:
    using Request = CoordinateCommitTransaction;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(ErrorCodes::CommandNotSupported,
                    "'coordinateCommitTransaction' is only supported in feature compatibility "
                    "version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();
            const auto tcs = TransactionCoordinatorService::get(opCtx);

            boost::optional<Future<txn::CommitDecision>> commitDecisionFuture;

            if (!cmd.getParticipants().empty()) {
                // Convert the participant list array into a set, and assert that all participants
                // in the list are unique.
                // TODO (PM-564): Propagate the 'readOnly' flag down into the
                // TransactionCoordinator.
                std::set<ShardId> participantList;
                StringBuilder ss;
                ss << "[";
                for (const auto& participant : cmd.getParticipants()) {
                    const auto& shardId = participant.getShardId();
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "participant list contained duplicate shardId "
                                          << shardId,
                            std::find(participantList.begin(), participantList.end(), shardId) ==
                                participantList.end());
                    participantList.insert(shardId);
                    ss << shardId << " ";
                }
                ss << "]";
                LOG(3) << "Coordinator shard received request to coordinate commit with "
                          "participant list "
                       << ss.str() << " for transaction " << opCtx->getTxnNumber() << " on session "
                       << opCtx->getLogicalSessionId()->toBSON();

                commitDecisionFuture = tcs->coordinateCommit(
                    opCtx, *opCtx->getLogicalSessionId(), *opCtx->getTxnNumber(), participantList);
            } else {
                LOG(3) << "Coordinator shard received request to recover commit decision for "
                          "transaction "
                       << opCtx->getTxnNumber() << " on session "
                       << opCtx->getLogicalSessionId()->toBSON();

                commitDecisionFuture = tcs->recoverCommit(
                    opCtx, *opCtx->getLogicalSessionId(), *opCtx->getTxnNumber());
            }

            ON_BLOCK_EXIT([opCtx] {
                // Since a commit decision will have been written from another OperationContext (by
                // either the coordinator or participant), ensure waiting for the client's
                // writeConcern of the decision.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
            });

            if (commitDecisionFuture) {
                // The commit coordination is still ongoing. Block waiting for the decision.
                auto commitDecision = commitDecisionFuture->get(opCtx);
                switch (commitDecision) {
                    case txn::CommitDecision::kCanceled:
                        // Continue on to recover the commit decision from disk.
                        break;
                    case txn::CommitDecision::kAbort:
                        uasserted(ErrorCodes::NoSuchTransaction, "Transaction was aborted");
                    case txn::CommitDecision::kCommit:
                        return;
                }
            }

            // No coordinator was found in memory. Either the commit coordination already completed,
            // the original primary on which the coordinator was created stepped down, or this
            // coordinateCommit request was a byzantine message.

            LOG(3) << "Coordinator shard going to attempt to recover decision from local "
                      "participant for transaction "
                   << opCtx->getTxnNumber() << " on session "
                   << opCtx->getLogicalSessionId()->toBSON();

            // Recover the decision from the local participant by sending abortTransaction to this
            // node and inverting the response (i.e., a success response is converted to
            // NoSuchTransaction; a TransactionCommitted response is converted to success). Do not
            // pass writeConcern; if the coordinateCommitTransaction command ends up throwing
            // NoSuchTransaction and the client sent a non-default writeConcern, the
            // coordinateCommitTransaction command's post-amble will do a no-op write and wait for
            // the client's writeConcern.
            BSONObj abortRequestObj =
                BSON("abortTransaction" << 1 << "lsid" << opCtx->getLogicalSessionId()->toBSON()
                                        << "txnNumber"
                                        << *opCtx->getTxnNumber()
                                        << "autocommit"
                                        << false);

            BSONObj abortResponseObj;

            const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            auto cbHandle = uassertStatusOK(executor->scheduleWork([
                serviceContext = opCtx->getServiceContext(),
                &abortResponseObj,
                abortRequestObj = abortRequestObj.getOwned()
            ](const executor::TaskExecutor::CallbackArgs& cbArgs) {
                ThreadClient threadClient(serviceContext);
                auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
                auto opCtx = uniqueOpCtx.get();

                AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx);

                auto requestOpMsg =
                    OpMsgRequest::fromDBAndBody(NamespaceString::kAdminDb, abortRequestObj)
                        .serialize();
                const auto replyOpMsg = OpMsg::parseOwned(serviceContext->getServiceEntryPoint()
                                                              ->handleRequest(opCtx, requestOpMsg)
                                                              .response);

                invariant(replyOpMsg.sequences.empty());
                abortResponseObj = replyOpMsg.body.getOwned();
            }));
            executor->wait(cbHandle, opCtx);

            const auto abortStatus = getStatusFromCommandResult(abortResponseObj);

            // Since the abortTransaction was sent without writeConcern, there should not be a
            // writeConcern error.
            invariant(getWriteConcernStatusFromCommandResult(abortResponseObj).isOK());

            LOG(3) << "coordinateCommitTransaction got response " << abortStatus << " for "
                   << abortRequestObj << " used to recover decision from local participant";

            // If the abortTransaction succeeded, return that the transaction aborted.
            uassert(ErrorCodes::NoSuchTransaction, "transaction aborted", !abortStatus.isOK());

            // If the abortTransaction returned that the transaction committed, return
            // ok, otherwise return whatever the abortTransaction errored with (which may be
            // NoSuchTransaction).
            uassert(abortStatus.code(),
                    abortStatus.reason(),
                    abortStatus.code() == ErrorCodes::TransactionCommitted);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Coordinates the commit for a transaction. Only called by mongos.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

} coordinateCommitTransactionCmd;

}  // namespace
}  // namespace mongo
