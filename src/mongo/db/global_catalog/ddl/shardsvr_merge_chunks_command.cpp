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


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/merge_chunks_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

/**
 * Attempts to execute the merge through the sharding coordinator service, retrying while a
 * conflicting coordinator is already running for the same namespace. Returns true if the merge
 * completed via the coordinator; returns false if the caller should fall back to the legacy
 * config-server path (because the authoritative metadata feature flag is disabled). Throws
 * ConflictingOperationInProgress if the configured retry budget is exhausted.
 */
bool tryRunMergeChunksCoordinator(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ShardsvrMergeChunks& req) {
    // If a conflicting merge coordinator is already running for this namespace,
    // wait for it to complete and retry.
    // TODO (SERVER-125033): Remove the retry-loop once this task gets done.
    const int maxConflictRetries = shardsvrMergeChunksMaxConflictRetries.load();
    Status lastConflictStatus = Status::OK();
    for (int retries = 0; retries < maxConflictRetries; ++retries) {
        boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};

        if (!feature_flags::gShardAuthoritativeCollMetadata.isEnabled(
                VersionContext::getDecoration(opCtx),
                optFixedFcvRegion.get()->acquireFCVSnapshot())) {
            return false;
        }

        auto coordinatorDoc = MergeChunksCoordinatorDocument();
        coordinatorDoc.setShardsvrMergeChunksRequest(req.getShardsvrMergeChunksRequest());
        coordinatorDoc.setShardingCoordinatorMetadata({{nss, CoordinatorTypeEnum::kMergeChunks}});

        // Defer option conflict checking to the explicit checkIfOptionsConflict
        // call below, allowing the retry loop to handle ConflictingOperationInProgress.
        auto service = ShardingCoordinatorService::getService(opCtx);
        auto coordinator =
            checked_pointer_cast<MergeChunksCoordinator>(service->getOrCreateInstance(
                opCtx, coordinatorDoc.toBSON(), *optFixedFcvRegion, false /*checkOptions*/));

        try {
            coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON());
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            LOGV2_DEBUG(12117904,
                        1,
                        "Merge chunks coordinator already running, waiting for completion",
                        "namespace"_attr = nss,
                        "error"_attr = ex);
            lastConflictStatus = ex.toStatus();
            optFixedFcvRegion.reset();
            coordinator->getCompletionFuture().getNoThrow(opCtx).ignore();
            continue;
        }

        optFixedFcvRegion.reset();
        coordinator->getCompletionFuture().get(opCtx);
        return true;
    }

    uasserted(ErrorCodes::ConflictingOperationInProgress,
              str::stream() << "Failed to execute merge chunks for namespace "
                            << nss.toStringForErrorMsg() << " after " << maxConflictRetries
                            << " retries due to conflicting operations. Last conflict: "
                            << lastConflictStatus.reason());
}

class ShardsvrMergeChunksCommand : public TypedCommand<ShardsvrMergeChunksCommand> {
public:
    using Request = ShardsvrMergeChunks;

    ShardsvrMergeChunksCommand() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command to merge a contiguous range of chunks.\n"
               "Usage: { _shardsvrMergeChunks: <ns>, epoch: <epoch>, bounds: [<min key>, <max "
               "key>] }";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            const auto& nss = ns();
            const auto& req = request();

            if (tryRunMergeChunksCoordinator(opCtx, nss, req)) {
                return;
            }

            // Legacy path: precondition checks + merge via config server.
            auto bounds = req.getBounds();
            uassertStatusOK(ChunkRange::validate(bounds));

            ChunkRange chunkRange(bounds[0], bounds[1]);

            auto scopedSplitOrMergeChunk(
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                    opCtx, nss, chunkRange)));

            auto expectedEpoch = req.getEpoch();
            auto expectedTimestamp = req.getTimestamp();

            const auto metadataBeforeMerge = [&]() {
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                        opCtx, nss, boost::none));
                const auto metadata =
                    checkCollectionIdentity(opCtx, nss, expectedEpoch, expectedTimestamp);
                checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
                checkRangeOwnership(opCtx, nss, metadata, chunkRange);
                return metadata;
            }();

            auto const shardingState = ShardingState::get(opCtx);

            ConfigSvrMergeChunks configRequest{
                nss, shardingState->shardId(), metadataBeforeMerge.getUUID(), chunkRange};
            configRequest.setEpoch(expectedEpoch);
            configRequest.setTimestamp(expectedTimestamp);
            configRequest.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

            auto cmdResponse =
                uassertStatusOK(Grid::get(opCtx)
                                    ->shardRegistry()
                                    ->getConfigShard()
                                    ->runCommandWithIndefiniteRetries(
                                        opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        configRequest.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            auto chunkVersionReceived = [&]() -> boost::optional<ChunkVersion> {
                if (cmdResponse.response[ChunkVersion::kChunkVersionField]) {
                    return ChunkVersion::parse(
                        cmdResponse.response[ChunkVersion::kChunkVersionField]);
                }
                return boost::none;
            }();
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, nss, std::move(chunkVersionReceived)));

            uassertStatusOKWithContext(cmdResponse.commandStatus, "Failed to commit chunk merge");
            uassertStatusOKWithContext(cmdResponse.writeConcernStatus,
                                       "Failed to commit chunk merge");
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
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
MONGO_REGISTER_COMMAND(ShardsvrMergeChunksCommand).forShard();

}  // namespace
}  // namespace mongo
