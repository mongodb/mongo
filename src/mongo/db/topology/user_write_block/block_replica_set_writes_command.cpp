// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/user_write_block/block_replica_set_writes_gen.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"
#include "mongo/db/version_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <mutex>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangInBlockReplicaSetWritesCommand);

class BlockReplicaSetWritesCommand final : public TypedCommand<BlockReplicaSetWritesCommand> {
public:
    using Request = BlockReplicaSetWrites;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Set whether writes are blocked to avoid running out of disk";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const FixedFCVRegion fixedFcvRegion(opCtx);
            const auto fcvSnapshot = fixedFcvRegion->acquireFCVSnapshot();
            // setFCV and replica set write block are mutually exclusive. Hold a
            // FixedFCVRegion for the duration of the command so that this serializes on the FCV
            // lock with setFeatureCompatibilityVersion, which enters its FCV change region in
            // exclusive mode. Reject here if an FCV transition is already in progress.
            // (Generic FCV reference): Calling isUpgradingOrDowngrading to know when the cluster is
            // in upgrading/downgrading state.
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Cannot run " << Request::kCommandName
                                  << " while in upgrading/downgrading FCV state",
                    !fcvSnapshot.isUpgradingOrDowngrading());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " not supported with the current FCV. Upgrade to the "
                                     "highest FCV to perform this operation.",
                    feature_flags::gFeatureFlagBlockReplicaSetWrites.isEnabled(
                        VersionContext::getDecoration(opCtx), fcvSnapshot));

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " cannot be run on standalones",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());

            // Only one attempt to change whether writes are blocked may make progress at once.
            std::lock_guard lock(_mutex);
            if (MONGO_unlikely(hangInBlockReplicaSetWritesCommand.shouldFail())) {
                LOGV2(12096400, "hangInBlockReplicaSetWritesCommand failpoint enabled");
                hangInBlockReplicaSetWritesCommand.pauseWhileSet(opCtx);
            }

            if (request().getEnabled()) {
                // Enable write blocking. allowDeletions is required when enabling the block.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'allowDeletions' is required when enabling "
                                      << Request::kCommandName,
                        request().getAllowDeletions().has_value());
                // Prevent new index builds from starting.
                auto writeBlockState = ReplicaSetWriteBlockState::get(opCtx);
                writeBlockState->enableUserIndexBuildBlocking();
                // Temporarily block new index builds during setup. Once the critical section
                // is committed the op observer takes over enforcement, so this is always reset.
                ScopeGuard guard([&]() { writeBlockState->disableUserIndexBuildBlocking(); });
                // Abort and wait for any ongoing index builds to finish.
                IndexBuildsCoordinator::get(opCtx)->abortIndexBuildsForWriteBlocking(opCtx);

                // Enable write blocking
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->acquireRecoverableCriticalSectionBlockingReplicaSetWrites(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::
                            kBlockReplicaSetWritesNamespace,
                        *request().getAllowDeletions(),
                        request().getReason());
            } else {
                // Disable write blocking.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'allowDeletions' is not allowed when disabling "
                                      << Request::kCommandName,
                        !request().getAllowDeletions().has_value());
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->releaseRecoverableCriticalSectionBlockingReplicaSetWrites(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::
                            kBlockReplicaSetWritesNamespace,
                        request().getReason());
            }

            // Wait for the write of the critical section to be majority committed.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            WriteConcernResult writeConcernResult;
            uassertStatusOK(waitForWriteConcern(opCtx,
                                                replClient.getLastOp(),
                                                defaultMajorityWriteConcernDoNotUse(),
                                                &writeConcernResult));
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::blockReplicaSetWrites}));
        }

        inline static std::mutex _mutex;
    };
};
MONGO_REGISTER_COMMAND(BlockReplicaSetWritesCommand)
    .requiresFeatureFlag(feature_flags::gFeatureFlagBlockReplicaSetWrites)
    .forShard();
}  // namespace
}  // namespace mongo
