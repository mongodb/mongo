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
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
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

        void setAllowChunkOperations(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     boost::optional<CollectionAcquisition> acq,
                                     bool allowChunkOperations,
                                     const boost::optional<UUID>& uuid) {
            shard_catalog_commit::commitSetAllowChunkOperationsLocally(
                opCtx, nss, allowChunkOperations, uuid);

            if (allowChunkOperations) {
                // If we are setting chunk operations back on, there's nothing to drain.
                return;
            }

            boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;

            {
                const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
                if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
                    waitForMigrationAbort.emplace(msm->abort());
                }
            }

            // Release the collection acquisition at this point. If some chunk operation (or
            // migration) was in the commit phase, we have to release the acquisition so that it can
            // acquire the critical section to attempt to commit. The commit should always fail,
            // because by the time this command is invoked, the CSRS has already stored
            // "allowMigrations: false", so it should reject any commit attempt.
            acq.reset();

            if (waitForMigrationAbort) {
                waitForMigrationAbort->get(opCtx);
            }

            // Wait for all chunk operation coordinators.
            auto* const service = ShardingCoordinatorService::getService(opCtx);
            for (const auto coordType : std::array{CoordinatorTypeEnum::kMoveRange,
                                                   CoordinatorTypeEnum::kSplitChunk,
                                                   CoordinatorTypeEnum::kMergeChunks,
                                                   CoordinatorTypeEnum::kMergeAllChunks}) {
                service->waitForCoordinatorsOfGivenTypeToComplete(opCtx, coordType);
            }
        }

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(12120900,
                    "Expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            const auto nss = ns();

            uassert(12120913,
                    "_shardsvrSetAllowChunkOperations should only run with AuthoritativeShardsDDL "
                    "enabled",
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                        AuthoritativeMetadataAccessLevelEnum::kNone);

            uassert(12120901,
                    "This command must be invoked following the shard protocol",
                    OperationShardingState::isVersioned(opCtx, nss));

            const auto allowChunkOperations = request().getAllowChunkOperations();
            const auto collectionUUID = request().getCollectionUUID();

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

                // ScopedSetShardRole to transfer the shard version to the new opCtx.
                ScopedSetShardRole scopedSetShardRole(
                    newOpCtx,
                    nss,
                    OperationShardingState::get(opCtx).getShardVersion(nss),
                    OperationShardingState::get(opCtx).getDbVersion(nss.dbName()));

                // DBDirectClient retries WriteConflictException only when yielding is allowed.
                // This command holds a CollectionAcquisition across the commit path, so yielding
                // is disabled and WriteConflictException would otherwise propagate.
                writeConflictRetry(newOpCtx, "ShardsvrSetAllowChunkOperations", nss, [&] {
                    // This command is invoked following the shard version protocol, so it is
                    // necessary to acquire the target collection to enforce it. Additionally,
                    // holding this acquisition guarantees serialization with the critical section
                    // (both the blocking writes CS and the blocking reads CS), so it is guaranteed
                    // that no chunk operation commits while this acquisition is held.
                    boost::optional<CollectionAcquisition> acq = acquireCollection(
                        newOpCtx,
                        CollectionAcquisitionRequest::fromOpCtx(
                            newOpCtx, nss, AcquisitionPrerequisites::OperationType::kWrite),
                        MODE_IX);

                    setAllowChunkOperations(
                        newOpCtx, nss, std::move(acq), allowChunkOperations, collectionUUID);
                });
            }

            LOGV2_INFO(12120902,
                       "setAllowChunkOperations finished",
                       "ns"_attr = nss,
                       "allowChunkOperations"_attr = allowChunkOperations);

            // Since no write happened on this txnNumber, we need to make a dummy write so that
            // secondaries can be aware of this txn.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << Request::kCommandName),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
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
