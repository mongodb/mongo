// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/split_chunk.h"
#include "mongo/db/global_catalog/ddl/split_chunk_coordinator.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
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
 * conflicting coordinator is already running for the same namespace. Returns boost::none if the
 * split completed via the coordinator; otherwise returns a FixedFCVRegion for the caller to
 * register through the legacy path under the same pin (because the authoritative metadata feature
 * is disabled). Throws ConflictingOperationInProgress if the configured retry budget is exhausted.
 */
boost::optional<FixedFCVRegion> tryRunSplitChunkCoordinator(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const ShardsvrSplitChunk& req) {
    // If a conflicting split coordinator is already running for this namespace,
    // wait for it to complete and retry.
    // TODO (SERVER-125033): Remove the retry-loop once this task gets done.
    const int maxConflictRetries = shardsvrSplitChunkMaxConflictRetries.load();
    Status lastConflictStatus = Status::OK();
    for (int retries = 0; retries < maxConflictRetries; ++retries) {
        boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};

        if (sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                VersionContext::getDecoration(opCtx),
                optFixedFcvRegion.get()->acquireFCVSnapshot()) ==
            AuthoritativeMetadataAccessLevelEnum::kNone) {
            return optFixedFcvRegion;
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
        return boost::none;
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

            auto fcvRegionForLegacyRegister = tryRunSplitChunkCoordinator(opCtx, nss, req);
            if (!fcvRegionForLegacyRegister) {
                return;
            }

            // Legacy path: precondition checks + splitChunk().
            auto chunkRange = ChunkRange(req.getMin(), req.getMax());
            uassertStatusOK(ChunkRange::validate(chunkRange));

            {
                uassertStatusOK(FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(
                    opCtx, nss, boost::none));
                const auto metadata =
                    checkCollectionIdentity(opCtx, nss, req.getEpoch(), req.getTimestamp());
                checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
                checkChunkMatchesRange(opCtx, nss, metadata, chunkRange);
            }

            auto scopedChunk =
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                    opCtx, nss, chunkRange));
            fcvRegionForLegacyRegister.reset();

            uassertStatusOK(splitChunk_nonAuth(
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
