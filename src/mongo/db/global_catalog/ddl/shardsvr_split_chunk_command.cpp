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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/split_chunk.h"
#include "mongo/db/global_catalog/ddl/split_chunk_coordinator.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"

#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Attempts to execute the split through the sharding coordinator service, retrying while a
 * conflicting coordinator is already running for the same namespace. Returns true if the split
 * completed via the coordinator; returns false if the caller should fall back to the legacy
 * config-server path (because the authoritative metadata feature flag is disabled). Throws
 * ConflictingOperationInProgress if the configured retry budget is exhausted.
 */
bool tryRunSplitChunkCoordinator(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const ShardsvrSplitChunk& req) {
    // If a conflicting split coordinator is already running for this namespace,
    // wait for it to complete and retry.
    // TODO (SERVER-125033): Remove the retry-loop once this task gets done.
    const int maxConflictRetries = shardsvrSplitChunkMaxConflictRetries.load();
    Status lastConflictStatus = Status::OK();
    for (int retries = 0; retries < maxConflictRetries; ++retries) {
        boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};

        if (!feature_flags::gShardAuthoritativeCollMetadata.isEnabled(
                VersionContext::getDecoration(opCtx),
                optFixedFcvRegion.get()->acquireFCVSnapshot())) {
            return false;
        }

        auto coordinatorDoc = SplitChunkCoordinatorDocument();
        coordinatorDoc.setShardsvrSplitChunkRequest(req.getShardsvrSplitChunkRequest());
        coordinatorDoc.setShardingCoordinatorMetadata({{nss, CoordinatorTypeEnum::kSplitChunk}});

        // Defer option conflict checking to the explicit checkIfOptionsConflict
        // call below, allowing the retry loop to handle ConflictingOperationInProgress.
        auto service = ShardingCoordinatorService::getService(opCtx);
        auto coordinator = checked_pointer_cast<SplitChunkCoordinator>(service->getOrCreateInstance(
            opCtx, coordinatorDoc.toBSON(), *optFixedFcvRegion, false /*checkOptions*/));

        try {
            coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON());
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            LOGV2_DEBUG(12117801,
                        1,
                        "Split chunk coordinator already running, waiting for completion",
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
              str::stream() << "Failed to execute split chunk for namespace "
                            << nss.toStringForErrorMsg() << " after " << maxConflictRetries
                            << " retries due to conflicting operations. Last conflict: "
                            << lastConflictStatus.reason());
}

class ShardsvrSplitChunkCommand final : public TypedCommand<ShardsvrSplitChunkCommand> {
public:
    using Request = ShardsvrSplitChunk;

    ShardsvrSplitChunkCommand() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    std::string help() const override {
        return "internal command usage only\n"
               "example:\n"
               " { _shardsvrSplitChunk: \"db.foo\", keyPattern: {a:1}, min: {a:100},"
               " max: {a:200}, splitKeys: [{a:150}] }";
    }

    bool skipApiVersionCheck() const override {
        return true;
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

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            const auto& nss = ns();
            const auto& req = request();

            if (tryRunSplitChunkCoordinator(opCtx, nss, req)) {
                return;
            }

            // Legacy path: precondition checks + splitChunk().
            auto chunkRange = ChunkRange(req.getMin(), req.getMax());
            uassertStatusOK(ChunkRange::validate(chunkRange));

            {
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                        opCtx, nss, boost::none));
                const auto metadata =
                    checkCollectionIdentity(opCtx, nss, req.getEpoch(), req.getTimestamp());
                checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
                checkChunkMatchesRange(opCtx, nss, metadata, chunkRange);
            }

            auto scopedChunk =
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                    opCtx, nss, chunkRange));

            uassertStatusOK(splitChunk(
                opCtx,
                nss,
                req.getKeyPattern(),
                chunkRange,
                std::vector<BSONObj>(req.getSplitKeys().begin(), req.getSplitKeys().end()),
                std::string{req.getFrom()},
                req.getEpoch(),
                req.getTimestamp(),
                scopedChunk));
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
MONGO_REGISTER_COMMAND(ShardsvrSplitChunkCommand).forShard();

}  // namespace
}  // namespace mongo
