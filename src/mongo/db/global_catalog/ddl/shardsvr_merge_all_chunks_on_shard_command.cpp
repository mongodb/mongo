/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_all_chunks_coordinator.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Attempts to execute the merge-all-chunks-on-shard operation through the sharding coordinator
 * service, retrying while a conflicting coordinator is already running for the same namespace.
 * Populates `response` and returns true if the merge completed via the coordinator; returns
 * false if the caller should fall back to the legacy config-server path (because the
 * authoritative metadata feature flag is disabled). Throws ConflictingOperationInProgress if
 * the configured retry budget is exhausted.
 */
bool tryRunMergeAllChunksCoordinator(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const ShardSvrMergeAllChunksOnShard& req,
                                     MergeAllChunksOnShardResponse* response) {
    // If a conflicting merge all chunks coordinator is already running for this namespace,
    // wait for it to complete and retry.
    // TODO (SERVER-125033): Remove the retry-loop once this task gets done.
    const int maxConflictRetries = shardsvrMergeAllChunksMaxConflictRetries.load();
    Status lastConflictStatus = Status::OK();
    for (int retries = 0; retries < maxConflictRetries; ++retries) {
        boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};

        if (sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                VersionContext::getDecoration(opCtx),
                optFixedFcvRegion.get()->acquireFCVSnapshot()) ==
            AuthoritativeMetadataAccessLevelEnum::kNone) {
            return false;
        }

        auto coordinatorDoc = MergeAllChunksCoordinatorDocument();
        coordinatorDoc.setShardsvrMergeAllChunksOnShardRequest(
            req.getShardsvrMergeAllChunksOnShardRequest());
        coordinatorDoc.setShardingCoordinatorMetadata(
            {{nss, CoordinatorTypeEnum::kMergeAllChunks}});

        // Defer option conflict checking to the explicit checkIfOptionsConflict
        // call below, allowing the retry loop to handle ConflictingOperationInProgress.
        auto service = ShardingCoordinatorService::getService(opCtx);
        auto coordinator =
            checked_pointer_cast<MergeAllChunksCoordinator>(service->getOrCreateInstance(
                opCtx, coordinatorDoc.toBSON(), *optFixedFcvRegion, false /*checkOptions*/));

        try {
            coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON());
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            LOGV2_DEBUG(12118002,
                        1,
                        "Merge all chunks coordinator already running, waiting for completion",
                        "namespace"_attr = nss,
                        "error"_attr = ex);
            lastConflictStatus = ex.toStatus();
            optFixedFcvRegion.reset();
            coordinator->getCompletionFuture().getNoThrow(opCtx).ignore();
            continue;
        }

        optFixedFcvRegion.reset();
        *response = coordinator->getResponse(opCtx);
        return true;
    }

    uasserted(ErrorCodes::ConflictingOperationInProgress,
              str::stream() << "Failed to execute merge all chunks for namespace "
                            << nss.toStringForErrorMsg() << " after " << maxConflictRetries
                            << " retries due to conflicting operations. Last conflict: "
                            << lastConflictStatus.reason());
}

class ShardSvrMergeAllChunksOnShardCommand final
    : public TypedCommand<ShardSvrMergeAllChunksOnShardCommand> {

    static inline IDLParserContext IDL_PARSER_CONTEXT{"MergeAllChunksOnShardResponse"};

public:
    using Request = ShardSvrMergeAllChunksOnShard;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked either by the config server or by the mongos to merge all "
               "contiguous chunks on a shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        MergeAllChunksOnShardResponse typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            const auto& nss = ns();
            const auto& req = request();

            // Resolve the shard identifier here so that both the coordinator and the legacy paths
            // use the resolved identifier.
            const auto resolvedShardId =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                    opCtx, req.getShard(), true /* allowNonShardIdIdentifiers */));

            // The coordinator uses the request document to check for conflicting operations, so we
            // need to update it with the resolved identifier if the shardId was not used.
            const auto& resolvedRequest = [&]() {
                if (resolvedShardId == req.getShard()) {
                    return req;
                }
                ShardSvrMergeAllChunksOnShard newRequest(nss);
                newRequest.setShard(resolvedShardId.toString());
                newRequest.setMaxNumberOfChunksToMerge(req.getMaxNumberOfChunksToMerge());
                newRequest.setMaxTimeProcessingChunksMS(req.getMaxTimeProcessingChunksMS());
                return newRequest;
            }();

            MergeAllChunksOnShardResponse response;
            if (tryRunMergeAllChunksCoordinator(opCtx, nss, resolvedRequest, &response)) {
                return response;
            }
            // Legacy path: acquire a namespace-wide guard in the active migrations registry so
            // concurrent split/merge operations on this namespace are serialized with this
            // mergeAllChunks (matching the MergeAllChunksCoordinator's _acquireLocksAsync). The
            // guard is released when `scopedSplitOrMergeChunk` goes out of scope below.
            auto scopedSplitOrMergeChunk(
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                    opCtx, nss, ChunkRange(kMinBSONKey, kMaxBSONKey))));


            // Because this is a non-authoritative update, we must mark the CSR metadata as
            // kNonAuthoritative so that the following refresh will fetch the metadata from the
            // config server. Leaving it kAuthoritative would short-circuit the refresh against the
            // durable shard catalog and keep the CSR pinned to the pre-mergeAllChunks version.
            // This must be done before starting the operation to ensure the CSR is left as
            // kNonAuthoritative in case of an unexpected failure.
            {
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, ns());
                scopedCsr->clearFilteringMetadata_nonAuthoritative(opCtx);
            }

            // Legacy path: forward directly to the config server.
            ConfigSvrCommitMergeAllChunksOnShard configSvrCommitMergeAllChunksOnShard(nss);
            configSvrCommitMergeAllChunksOnShard.setDbName(DatabaseName::kAdmin);
            configSvrCommitMergeAllChunksOnShard.setShard(resolvedShardId.toString());
            configSvrCommitMergeAllChunksOnShard.setMaxNumberOfChunksToMerge(
                req.getMaxNumberOfChunksToMerge());
            configSvrCommitMergeAllChunksOnShard.setMaxTimeProcessingChunksMS(
                req.getMaxTimeProcessingChunksMS());
            configSvrCommitMergeAllChunksOnShard.setWriteConcern(
                defaultMajorityWriteConcernDoNotUse());

            auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto swCommandResponse =
                config->runCommand(opCtx,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   DatabaseName::kAdmin,
                                   configSvrCommitMergeAllChunksOnShard.toBSON(),
                                   Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swCommandResponse));

            auto res = MergeAllChunksOnShardResponse::parse(swCommandResponse.getValue().response,
                                                            IDL_PARSER_CONTEXT);

            // Update the shard catalog filtering metadata to reflect the new shard
            // version produced by the config server merge.
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, ns(), res.getShardVersion()));

            return res;
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
MONGO_REGISTER_COMMAND(ShardSvrMergeAllChunksOnShardCommand).forShard();

}  // namespace
}  // namespace mongo
