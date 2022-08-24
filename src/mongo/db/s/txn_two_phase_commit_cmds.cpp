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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterStartingCoordinateCommit);
MONGO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic);

class PrepareTransactionCmd : public TypedCommand<PrepareTransactionCmd> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool isTransactionCommand() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

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

            // If a node has majority read concern disabled, replication must use the legacy
            // 'rollbackViaRefetch' algortithm, which does not support prepareTransaction oplog
            // entries
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "'prepareTransaction' is not supported with 'enableMajorityReadConcern=false'",
                    serverGlobalParams.enableMajorityReadConcern);

            // Replica sets with arbiters are able to continually accept majority writes without
            // actually being able to commit them (e.g. PSA with a downed secondary), which in turn
            // will impact the liveness of 2PC transactions
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "'prepareTransaction' is not supported for replica sets with arbiters",
                    !replCoord->setContainsArbiter());

            // Standalone nodes do not support transactions at all
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "'prepareTransaction' is not supported on standalone nodes.",
                    replCoord->isReplEnabled());

            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "prepareTransaction must be run within a transaction",
                    txnParticipant);

            TxnNumberAndRetryCounter txnNumberAndRetryCounter{*opCtx->getTxnNumber(),
                                                              *opCtx->getTxnRetryCounter()};

            LOGV2_DEBUG(22483,
                        3,
                        "{sessionId}:{txnNumberAndRetryCounter} Participant shard received "
                        "prepareTransaction",
                        "Participant shard received prepareTransaction",
                        "sessionId"_attr = opCtx->getLogicalSessionId()->toBSON(),
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

            // TODO(SERVER-46105) remove
            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "Cannot create new collections inside distributed transactions",
                    UncommittedCatalogUpdates::get(opCtx).isEmpty());

            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction isn't in progress",
                    txnParticipant.transactionIsOpen());

            if (txnParticipant.transactionIsPrepared()) {
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                auto prepareOpTime = txnParticipant.getPrepareOpTime();

                // Ensure waiting for writeConcern of the prepare OpTime. If the node has failed
                // over, then we want to wait on an OpTime in the new term, so we wait on the
                // lastApplied OpTime. If we've gotten to this point, then we are guaranteed that
                // the transaction was prepared at this prepareOpTime on this branch of history and
                // that waiting on this lastApplied OpTime waits on the prepareOpTime as well.
                // Because lastAppliedOpTime is updated asynchronously with the WUOW that prepares
                // the transaction, there is a chance that the lastAppliedOpTime is behind the
                // prepareOpTime. And we also need to be careful not to set lastOp backwards. Thus,
                // we set the client lastOp to max of prepareOpTime, lastAppliedOpTime, and the
                // current lastOp.
                const auto lastAppliedOpTime =
                    repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
                replClient.setLastOp(
                    opCtx, std::max({prepareOpTime, lastAppliedOpTime, replClient.getLastOp()}));

                invariant(
                    opCtx->recoveryUnit()->getPrepareTimestamp() == prepareOpTime.getTimestamp(),
                    str::stream() << "recovery unit prepareTimestamp: "
                                  << opCtx->recoveryUnit()->getPrepareTimestamp().toString()
                                  << " participant prepareOpTime: " << prepareOpTime.toString());

                if (MONGO_unlikely(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic
                                       .shouldFail())) {
                    uasserted(ErrorCodes::HostUnreachable,
                              "returning network error because failpoint is on");
                }
                return PrepareTimestamp(prepareOpTime.getTimestamp());
            }

            const auto prepareTimestamp = txnParticipant.prepareTransaction(opCtx, {});
            if (MONGO_unlikely(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic
                                   .shouldFail())) {
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

    LOGV2_DEBUG(
        22484,
        3,
        "{sessionId}:{txnNumber} Coordinator shard received request to coordinate commit with "
        "participant list {participantList}",
        "Coordinator shard received request to coordinate commit",
        "sessionId"_attr = opCtx->getLogicalSessionId()->getId(),
        "txnNumber"_attr = opCtx->getTxnNumber(),
        "txnRetryCounter"_attr = opCtx->getTxnRetryCounter(),
        "participantList"_attr = ss.str());

    return participantsSet;
}

class CoordinateCommitTransactionCmd : public TypedCommand<CoordinateCommitTransactionCmd> {
public:
    using Request = CoordinateCommitTransaction;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            const auto& cmd = request();
            const auto tcs = TransactionCoordinatorService::get(opCtx);

            const TxnNumberAndRetryCounter txnNumberAndRetryCounter{*opCtx->getTxnNumber(),
                                                                    *opCtx->getTxnRetryCounter()};

            // Coordinate the commit, or recover the commit decision from disk if this command was
            // sent without a participant list.
            auto coordinatorDecisionFuture = cmd.getParticipants().empty()
                ? tcs->recoverCommit(opCtx, *opCtx->getLogicalSessionId(), txnNumberAndRetryCounter)
                : tcs->coordinateCommit(opCtx,
                                        *opCtx->getLogicalSessionId(),
                                        txnNumberAndRetryCounter,
                                        validateParticipants(opCtx, cmd.getParticipants()));

            if (MONGO_unlikely(hangAfterStartingCoordinateCommit.shouldFail())) {
                LOGV2(22485, "Hit hangAfterStartingCoordinateCommit failpoint");
                hangAfterStartingCoordinateCommit.pauseWhileSet(opCtx);
            }

            ON_BLOCK_EXIT([opCtx] {
                // A decision will most likely have been written from a different OperationContext
                // (in all cases except the one where this command aborts the local participant), so
                // ensure waiting for the client's writeConcern of the decision.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTimeIgnoringShutdownCtxCancelled(opCtx);
            });

            if (coordinatorDecisionFuture) {
                auto swCommitDecision = coordinatorDecisionFuture->getNoThrow(opCtx);

                // The coordinator can throw TransactionCoordinatorCanceled if
                // cancelIfCommitNotYetStarted was called, which can happen in one of 3 cases:
                //
                //  1) The deadline to receive coordinateCommit passed
                //  2) Transaction with a newer txnNumber started on the session before
                //     coordinateCommit was received
                //  3) This is a sharded transaction, which used the optimized commit path and
                //     didn't require 2PC
                //
                // Even though only (3) requires recovering the commit decision from the local
                // participant, since these cases cannot be differentiated currently, we always
                // recover from the local participant.
                if (swCommitDecision != ErrorCodes::TransactionCoordinatorCanceled) {
                    if (swCommitDecision.isOK()) {
                        invariant(swCommitDecision.getValue() == txn::CommitDecision::kCommit);
                        return;
                    }

                    invariant(swCommitDecision != ErrorCodes::TransactionCoordinatorSteppingDown);
                    invariant(swCommitDecision !=
                              ErrorCodes::TransactionCoordinatorReachedAbortDecision);

                    uassertStatusOKWithContext(swCommitDecision, "Transaction was aborted");
                }
            }

            // No coordinator was found in memory. Recover the decision from the local participant.

            LOGV2_DEBUG(22486,
                        3,
                        "{sessionId}:{txnNumberAndRetryCounter} Going to recover decision from "
                        "local participant",
                        "Going to recover decision from local participant",
                        "sessionId"_attr = opCtx->getLogicalSessionId()->getId(),
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

            boost::optional<SharedSemiFuture<void>> participantExitPrepareFuture;
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            {
                auto sessionTxnState = mongoDSessionCatalog->checkOutSession(opCtx);
                auto txnParticipant = TransactionParticipant::get(opCtx);
                txnParticipant.beginOrContinue(opCtx,
                                               txnNumberAndRetryCounter,
                                               false /* autocommit */,
                                               boost::none /* startTransaction */);

                if (txnParticipant.transactionIsCommitted())
                    return;
                if (txnParticipant.transactionIsInProgress()) {
                    txnParticipant.abortTransaction(opCtx);
                }

                participantExitPrepareFuture = txnParticipant.onExitPrepare();
            }

            // Wait for the participant to exit prepare.
            participantExitPrepareFuture->get(opCtx);

            {
                auto sessionTxnState = mongoDSessionCatalog->checkOutSession(opCtx);
                auto txnParticipant = TransactionParticipant::get(opCtx);

                // Call beginOrContinue again in case the transaction number has changed.
                txnParticipant.beginOrContinue(opCtx,
                                               txnNumberAndRetryCounter,
                                               false /* autocommit */,
                                               boost::none /* startTransaction */);

                invariant(!txnParticipant.transactionIsOpen(),
                          "The participant should not be in progress after we waited for the "
                          "participant to complete");
                uassert(ErrorCodes::NoSuchTransaction,
                        "Recovering the transaction's outcome found the transaction aborted",
                        txnParticipant.transactionIsCommitted());
            }
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

    bool isTransactionCommand() const final {
        return true;
    }

    bool shouldCheckoutSession() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

} coordinateCommitTransactionCmd;

}  // namespace
}  // namespace mongo
