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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
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

                // Ensure waiting for writeConcern of the prepare OpTime.
                if (prepareOpTime > replClient.getLastOp()) {
                    // In case this node has failed over, in which case the term will have
                    // increased, set the Client's last OpTime to the larger of the system last
                    // OpTime and the prepare OpTime.
                    const auto systemLastOpTime =
                        repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
                    replClient.setLastOp(std::max(prepareOpTime, systemLastOpTime));
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

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                             ActionType::internal}));
        }
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

std::set<ShardId> validateParticipants(OperationContext* opCtx,
                                       const std::vector<mongo::CommitParticipant>& participants) {
    StringBuilder ss;
    std::set<ShardId> participantsSet;

    ss << '[';
    for (const auto& participant : participants) {
        const auto& shardId = participant.getShardId();
        const bool inserted = participantsSet.emplace(shardId).second;
        uassert(51162,
                str::stream() << "Participant list contains duplicate shard " << shardId,
                inserted);
        ss << shardId << ", ";
    }
    ss << ']';

    LOG(3) << "Coordinator shard received request to coordinate commit with "
              "participant list "
           << ss.str() << " for " << opCtx->getLogicalSessionId()->getId() << ':'
           << opCtx->getTxnNumber();

    return participantsSet;
}

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
                commitDecisionFuture =
                    tcs->coordinateCommit(opCtx,
                                          *opCtx->getLogicalSessionId(),
                                          *opCtx->getTxnNumber(),
                                          validateParticipants(opCtx, cmd.getParticipants()));
            } else {
                LOG(3) << "Coordinator shard received request to recover commit decision for "
                       << opCtx->getLogicalSessionId()->getId() << ':' << opCtx->getTxnNumber();

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
                auto swCommitDecision = commitDecisionFuture->getNoThrow(opCtx);
                // The coordinator can only return NoSuchTransaction if cancelIfCommitNotYetStarted
                // was called, which can happen in one of 3 cases:
                //  1) The deadline to receive coordinateCommit passed
                //  2) Transaction with a newer txnNumber started on the session before
                //     coordinateCommit was received
                //  3) This is a sharded transaction, which used the optimized commit path and
                //     didn't require 2PC
                //
                // Even though only (3) requires recovering the commit decision from the local
                // participant, since these cases cannot be differentiated currently, we always
                // recover from the local participant.
                if (swCommitDecision != ErrorCodes::NoSuchTransaction) {
                    auto commitDecision = uassertStatusOK(std::move(swCommitDecision));
                    switch (commitDecision) {
                        case txn::CommitDecision::kCommit:
                            return;
                        case txn::CommitDecision::kAbort:
                            uasserted(ErrorCodes::NoSuchTransaction, "Transaction was aborted");
                    }
                }
            }

            // No coordinator was found in memory. Either the commit coordination already completed,
            // the original primary on which the coordinator was created stepped down, or this
            // coordinateCommit request was a byzantine message.

            LOG(3) << "Coordinator shard going to attempt to recover decision from local "
                      "participant for "
                   << opCtx->getLogicalSessionId()->getId() << ':' << opCtx->getTxnNumber();

            // Recover the decision from the local participant by sending abortTransaction to this
            // node and inverting the response (i.e., a success response is converted to
            // NoSuchTransaction; a TransactionCommitted response is converted to success). Do not
            // pass writeConcern; if the coordinateCommitTransaction command ends up throwing
            // NoSuchTransaction and the client sent a non-default writeConcern, the
            // coordinateCommitTransaction command's post-amble will do a no-op write and wait for
            // the client's writeConcern.
            AbortTransaction abortTransaction;
            abortTransaction.setDbName(NamespaceString::kAdminDb);
            auto abortObj = abortTransaction.toBSON(
                BSON("lsid" << opCtx->getLogicalSessionId()->toBSON() << "txnNumber"
                            << *opCtx->getTxnNumber()
                            << "autocommit"
                            << false));

            const auto abortStatus = [&] {
                txn::AsyncWorkScheduler aws(opCtx->getServiceContext());
                auto future =
                    aws.scheduleRemoteCommand(txn::getLocalShardId(opCtx->getServiceContext()),
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              abortObj);
                ON_BLOCK_EXIT([&] {
                    aws.shutdown({ErrorCodes::Interrupted, "Request interrupted due to timeout"});
                    future.wait();
                });
                const auto& responseStatus = future.get(opCtx);
                uassertStatusOK(responseStatus.status);

                return getStatusFromCommandResult(responseStatus.data);
            }();

            LOG(3) << "coordinateCommitTransaction got response " << abortStatus << " for "
                   << abortObj << " used to recover decision from local participant";

            // If the abortTransaction succeeded, return that the transaction aborted.
            if (abortStatus.isOK())
                uasserted(ErrorCodes::NoSuchTransaction, "Transaction aborted");

            // If the abortTransaction returned that the transaction committed, return OK, otherwise
            // return whatever the abortTransaction errored with (which may be NoSuchTransaction).
            if (abortStatus != ErrorCodes::TransactionCommitted)
                uassertStatusOK(abortStatus);
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
