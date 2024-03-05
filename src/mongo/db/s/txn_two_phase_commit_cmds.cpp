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


#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

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

    using Request = PrepareTransaction;
    using Response = PrepareReply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            if (!getTestCommandsEnabled() &&
                !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
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
                    replCoord->getSettings().isReplSet());

            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "prepareTransaction must be run within a transaction",
                    txnParticipant);

            auto txnRouter = TransactionRouter::get(opCtx);
            if (txnRouter) {
                auto nss = ns();
                auto additionalParticipants = txnRouter.getAdditionalParticipantsForResponse(
                    opCtx, definition()->getName(), nss);
                if (additionalParticipants) {
                    for (const auto& p : *additionalParticipants) {
                        uassert(ErrorCodes::IllegalOperation,
                                str::stream()
                                    << "Cannot prepare transaction because this shard added "
                                       "participant(s) to this transaction, and did not "
                                       "receive a response from additional participant: "
                                    << p.first,
                                p.second);
                    }
                }
            }

            TxnNumberAndRetryCounter txnNumberAndRetryCounter{*opCtx->getTxnNumber(),
                                                              *opCtx->getTxnRetryCounter()};

            LOGV2_DEBUG(22483,
                        3,
                        "Participant shard received prepareTransaction",
                        "sessionId"_attr = opCtx->getLogicalSessionId()->toBSON(),
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

            if (!feature_flags::gCreateCollectionInPreparedTransactions.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                uassert(ErrorCodes::OperationNotSupportedInTransaction,
                        "Cannot create new collections inside distributed transactions",
                        UncommittedCatalogUpdates::get(opCtx).isEmpty());
            } else {
                // TODO SERVER-81037: This can be removed whenever the catalog uses the new schema
                // and we can rely on WT to detect these changes.
                //
                // We now verify that the created collections are not part of the latest catalog.
                // That means that there is a prepare conflict and we should error.
                auto latestCatalog = CollectionCatalog::latest(opCtx);
                const auto& updates = UncommittedCatalogUpdates::get(opCtx);
                for (const auto& update : updates.entries()) {
                    if (update.action !=
                        UncommittedCatalogUpdates::Entry::Action::kCreatedCollection) {
                        continue;
                    }
                    // TODO SERVER-81937: Verify that the DDL Coordinator locks are acquired for all
                    // uncommitted collection catalog entries.

                    latestCatalog->ensureCollectionIsNew(opCtx, update.nss);
                }
            }

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

                invariant(shard_role_details::getRecoveryUnit(opCtx)->getPrepareTimestamp() ==
                              prepareOpTime.getTimestamp(),
                          str::stream()
                              << "recovery unit prepareTimestamp: "
                              << shard_role_details::getRecoveryUnit(opCtx)
                                     ->getPrepareTimestamp()
                                     .toString()
                              << " participant prepareOpTime: " << prepareOpTime.toString());

                if (MONGO_unlikely(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic
                                       .shouldFail())) {
                    uasserted(ErrorCodes::HostUnreachable,
                              "returning network error because failpoint is on");
                }
                return createResponse(prepareOpTime.getTimestamp(),
                                      txnParticipant.affectedNamespaces());
            }

            auto [prepareTimestamp, affectedNamespaces] =
                txnParticipant.prepareTransaction(opCtx, {});
            if (MONGO_unlikely(participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic
                                   .shouldFail())) {
                uasserted(ErrorCodes::HostUnreachable,
                          "returning network error because failpoint is on");
            }
            return createResponse(std::move(prepareTimestamp), std::move(affectedNamespaces));
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        Response createResponse(Timestamp prepareTimestamp,
                                absl::flat_hash_set<NamespaceString> affectedNamespaces) {
            Response response;
            response.setPrepareTimestamp(std::move(prepareTimestamp));
            if (feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                std::vector<NamespaceString> namespaces;
                namespaces.reserve(affectedNamespaces.size());
                std::move(affectedNamespaces.begin(),
                          affectedNamespaces.end(),
                          std::back_inserter(namespaces));
                response.setAffectedNamespaces(std::move(namespaces));
            }
            return response;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
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
};
MONGO_REGISTER_COMMAND(PrepareTransactionCmd).forShard();

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

    LOGV2_DEBUG(22484,
                3,
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
            if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
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
                    .setLastOpToSystemLastOpTimeIgnoringCtxInterrupted(opCtx);
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
                        "Going to recover decision from local participant",
                        "sessionId"_attr = opCtx->getLogicalSessionId()->getId(),
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

            boost::optional<SharedSemiFuture<void>> participantExitPrepareFuture;
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            {
                auto sessionTxnState = mongoDSessionCatalog->checkOutSession(opCtx);
                auto txnParticipant = TransactionParticipant::get(opCtx);
                txnParticipant.beginOrContinue(
                    opCtx,
                    txnNumberAndRetryCounter,
                    false /* autocommit */,
                    TransactionParticipant::TransactionActions::kContinue);

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
                txnParticipant.beginOrContinue(
                    opCtx,
                    txnNumberAndRetryCounter,
                    false /* autocommit */,
                    TransactionParticipant::TransactionActions::kContinue);

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
            return NamespaceString(request().getDbName());
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
};
MONGO_REGISTER_COMMAND(CoordinateCommitTransactionCmd).forShard();

}  // namespace
}  // namespace mongo
