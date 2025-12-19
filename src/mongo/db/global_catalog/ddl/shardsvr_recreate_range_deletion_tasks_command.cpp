/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter


namespace mongo {
namespace {

constexpr StringData kLockReason = "RecreateRangeDeletionTasks"_sd;

class ShardSvrRecreateRangeDeletionTasksCommand final
    : public TypedCommand<ShardSvrRecreateRangeDeletionTasksCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Recreating range deletion task documents for a collection "
               "on all shards, according to the routing table.";
    }

    using Request = ShardSvrRecreateRangeDeletionTasks;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto& nss = ns();

            // Acquire DDL lock to ensure collection stability while recreating range deletion docs
            DDLLockManager::ScopedCollectionDDLLock dbDDLLock{opCtx, nss, kLockReason, MODE_X};

            const auto collectionUuid = [&] {
                const auto collection = acquireCollectionMaybeLockFree(
                    opCtx,
                    CollectionAcquisitionRequest(nss,
                                                 PlacementConcern::kPretendUnsharded,
                                                 repl::ReadConcernArgs::get(opCtx),
                                                 AcquisitionPrerequisites::kRead));
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream()
                            << "Collection " << nss.toStringForErrorMsg() << " does not exist",
                        collection.exists());
                return collection.uuid();
            }();

            ShardSvrRecreateRangeDeletionTasksParticipant req{
                nss, request().getSkipEmptyRanges(), collectionUuid};

            auto shardResponses = scatterGatherUnversionedTargetAllShards(
                opCtx,
                nss.dbName(),
                applyReadWriteConcern(opCtx, this, req.toBSON()),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent);

            for (const auto& response : shardResponses) {
                uassertStatusOK(response.swResponse.getStatus());
                const auto& cmdResponse = response.swResponse.getValue();
                try {
                    uassertStatusOK(getStatusFromCommandResult(cmdResponse.data));
                } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
                    // Ignore collection UUID mismatch errors: no need to create range deletion
                    // documents on shards that don't know the collection or host an incarnation
                    // inconsistent with respect to the primary shard.
                }
            }
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::internal));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};
MONGO_REGISTER_COMMAND(ShardSvrRecreateRangeDeletionTasksCommand).forShard();

class ShardSvrRecreateRangeDeletionTasksParticipantCommand final
    : public TypedCommand<ShardSvrRecreateRangeDeletionTasksParticipantCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Recreating range deletion task documents for a collection on a"
               "specific shard, according to the routing table.";
    }

    using Request = ShardSvrRecreateRangeDeletionTasksParticipant;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto& nss = ns();
            const auto& expectedCollectionUuid = request().getUuid();
            bool shouldSkipEmptyRanges = request().getSkipEmptyRanges();

            // Prevent migrations from running while recreating range deletion documents
            auto& activeMigrationsRegistry = ActiveMigrationsRegistry::get(opCtx);
            activeMigrationsRegistry.lock(opCtx, kLockReason);
            ScopeGuard unlockActiveMigrationRegistry(
                [&activeMigrationsRegistry] { activeMigrationsRegistry.unlock(kLockReason); });

            const auto metadata = _getCollectionMetadata(opCtx, nss, expectedCollectionUuid);

            // If sharded metadata are available, we can rely on the current state (at most the
            // shard may have stale knowledge about unowned ranges, but what matters is that it
            // knows they're not owned, no matter where they're placed).
            if (metadata->isSharded()) {
                const ShardId donorShardId{"RecreateRangeDeletionTasks"};
                const auto shardKey = KeyPattern(metadata->getKeyPattern());
                const auto preMigrationShardVersion = ChunkVersion::IGNORED();

                const auto emptyChunkMap =
                    RangeMap{SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()};
                BSONObj lookupKey = metadata->getMinKey();
                boost::optional<ChunkRange> range;
                while ((range = metadata->getNextOrphanRange(emptyChunkMap, lookupKey))) {
                    lookupKey = range->getMax();

                    if (shouldSkipEmptyRanges) {
                        boost::optional<BSONObj> firstDocInRange =
                            MigrationDestinationManager::checkForExistingDocumentsInRange(
                                opCtx,
                                nss,
                                expectedCollectionUuid,
                                shardKey.toBSON(),
                                range->getMin(),
                                range->getMax());
                        if (!firstDocInRange.has_value()) {
                            // Do not persist range deletion document for empty range
                            continue;
                        }
                    }

                    rangedeletionutil::createAndPersistRangeDeletionTask(
                        opCtx,
                        UUID::gen() /* random task uuid (no migration id) */,
                        nss,
                        expectedCollectionUuid,
                        donorShardId,
                        *range,
                        CleanWhenEnum::kNow,
                        false /* pending */,
                        shardKey,
                        preMigrationShardVersion,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        true /* doNotPersistIfDocCoveringSameRangeAlreadyExists */);
                }
            }
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::internal));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

    private:
        /**
         * Retry loop to retrieve collection metadata. This method will throw if the collection does
         * not exist locally, avoiding unnecessary refreshes.
         */
        static boost::optional<CollectionMetadata> _getCollectionMetadata(
            OperationContext* opCtx,
            const NamespaceString& nss,
            const UUID& expectedCollectionUuid) {
            while (true) {
                const auto metadata = [&] {
                    const auto collection = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest(nss,
                                                     expectedCollectionUuid,
                                                     PlacementConcern::kPretendUnsharded,
                                                     repl::ReadConcernArgs::get(opCtx),
                                                     AcquisitionPrerequisites::kRead),
                        MODE_IS);
                    const auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx,
                                                                                          nss);
                    return scopedCsr->getCurrentMetadataIfKnown();
                }();

                if (metadata.has_value()) {
                    return metadata;
                }

                FilteringMetadataCache::get(opCtx)
                    ->onCollectionPlacementVersionMismatch(opCtx, nss, boost::none)
                    .ignore();
            }

            MONGO_UNREACHABLE;
        }
    };
};
MONGO_REGISTER_COMMAND(ShardSvrRecreateRangeDeletionTasksParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
