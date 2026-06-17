/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrSetAllowChunkOperationsCommand final
    : public TypedCommand<ShardsvrSetAllowChunkOperationsCommand> {
public:
    using Request = ShardsvrSetAllowChunkOperations;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Enable/disable chunk operations in a "
               "collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        boost::optional<SharedSemiFuture<void>> setAllowChunkOperations(
            OperationContext* opCtx,
            const NamespaceString& nss,
            bool allowChunkOperations,
            const boost::optional<UUID>& uuid,
            bool isPrimaryShard) {

            // Holding this acquisition guarantees serialization with the critical section (both the
            // blocking writes CS and the blocking reads CS), so it is guaranteed that no chunk
            // operation commits while this acquisition is held.
            // We release the collection acquisition when returning from this function, and drain
            // operations outside. If some chunk operation (or migration) was in the commit phase,
            // we have to release the acquisition so that it can acquire the critical section to
            // attempt to commit, otherwise we would deadlock.
            const CollectionAcquisition acq =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      opCtx, nss, AcquisitionPrerequisites::OperationType::kWrite),
                                  MODE_IX);

            shard_catalog_commit::commitSetAllowChunkOperationsLocally(
                opCtx, nss, allowChunkOperations, uuid, isPrimaryShard);

            if (allowChunkOperations) {
                return boost::none;
            }

            const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
            if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
                return msm->abort();
            }

            return boost::none;
        }

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(12120900,
                    "Expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            uassert(12120913,
                    "_shardsvrSetAllowChunkOperations should only run with AuthoritativeShardsDDL "
                    "enabled",
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                        AuthoritativeMetadataAccessLevelEnum::kNone);

            const auto nss = ns();
            const auto allowChunkOperations = request().getAllowChunkOperations();
            const auto collectionUUID = request().getCollectionUUID();
            boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;

            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrSetAllowChunkOperations");
                AlternativeClientRegion acr(newClient);
                auto newOpCtxContainer = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                auto* const newOpCtx = newOpCtxContainer.get();
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                AuthorizationSession::get(newOpCtx->getClient())->grantInternalAuthorization();
                newOpCtx->setWriteConcern(opCtx->getWriteConcern());

                // Enter the shard role with an IGNORED shard version. This ensures that
                // acquisitions are considered "versioned" and will honor the critical section, but
                // no actual version check will be performed.
                ScopedSetShardRole scopedSetShardRole(
                    newOpCtx, nss, ShardVersionFactory::make(ChunkVersion::IGNORED()), boost::none);

                // DBDirectClient retries WriteConflictException only when yielding is allowed.
                // This command holds a CollectionAcquisition across the commit path, so yielding
                // is disabled and WriteConflictException would otherwise propagate.
                waitForMigrationAbort =
                    writeConflictRetry(newOpCtx, "ShardsvrSetAllowChunkOperations", nss, [&] {
                        return setAllowChunkOperations(newOpCtx,
                                                       nss,
                                                       allowChunkOperations,
                                                       collectionUUID,
                                                       ShardingState::get(newOpCtx)->shardId() ==
                                                           request().getPrimaryShardId());
                    });
            }

            {
                // Since no write happened on this txnNumber, we need to make a dummy write so that
                // secondaries can be aware of this txn.
                DBDirectClient client(opCtx);
                client.update(NamespaceString::kServerConfigurationNamespace,
                              BSON("_id" << Request::kCommandName),
                              BSON("$inc" << BSON("count" << 1)),
                              true /* upsert */,
                              false /* multi */);
            }

            if (allowChunkOperations) {
                // If we are setting chunk operations back on, there's nothing to drain.
                invariant(!waitForMigrationAbort.has_value());
                return;
            }

            // Yield the session before waiting for ongoing operations, to avoid deadlocks. By this
            // point, all mutating operations have already been done, so there isn't any risk from
            // retried commands on this session running concurrently to this wait.
            auto* const mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            mongoDSessionCatalog->checkInUnscopedSession(
                opCtx, OperationContextSession::CheckInReason::kYield);

            if (waitForMigrationAbort) {
                waitForMigrationAbort->get(opCtx);
            }

            // Wait for all chunk operation coordinators.
            auto* const service = ShardingCoordinatorService::getService(opCtx);
            service->waitForOngoingCoordinatorsToFinish(
                opCtx, [&](const ShardingCoordinator& coord) -> bool {
                    static constexpr std::array kChunkOperationTypes{
                        CoordinatorTypeEnum::kMoveRange,
                        CoordinatorTypeEnum::kSplitChunk,
                        CoordinatorTypeEnum::kMergeChunks,
                        CoordinatorTypeEnum::kMergeAllChunks,
                    };
                    const auto type = coord.operationType();
                    const auto& coordNss = coord.originalNss();
                    return std::ranges::any_of(kChunkOperationTypes,
                                               [type](auto t) { return t == type; }) &&
                        (coordNss == nss || coordNss.makeTimeseriesBucketsNamespace() == nss);
                });

            // Checkout the session to check for potential newer commands on this session that may
            // have executed while we were waiting. If that's the case, this command should fail.
            opCtx->checkForInterrupt();
            mongoDSessionCatalog->checkOutUnscopedSession(opCtx);
            TransactionParticipant::get(opCtx).beginOrContinue(
                opCtx,
                {*opCtx->getTxnNumber()},
                boost::none /* autocommit */,
                TransactionParticipant::TransactionActions::kNone);

            LOGV2_INFO(12120902,
                       "setAllowChunkOperations finished",
                       "ns"_attr = nss,
                       "allowChunkOperations"_attr = allowChunkOperations);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrSetAllowChunkOperationsCommand).forShard();

}  // namespace
}  // namespace mongo
