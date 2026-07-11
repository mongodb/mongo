// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/user_write_block/global_user_write_block_state.h"
#include "mongo/db/topology/user_write_block/set_user_write_block_mode_gen.h"
#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <mutex>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

class SetUserWriteBlockModeCommand final : public TypedCommand<SetUserWriteBlockModeCommand> {
public:
    using Request = SetUserWriteBlockMode;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Set whether user write blocking is enabled";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " cannot be run on standalones",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());

            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, {UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace});

            // Only one attempt to change write block mode may make progress at once, because the
            // way we enable/disable user index build blocking is not concurrency-safe.
            std::lock_guard lock(_mutex);
            {
                if (request().getGlobal()) {
                    // Enabling write block mode on a replicaset requires several steps
                    // First, we must prevent new index builds from starting
                    auto writeBlockState = GlobalUserWriteBlockState::get(opCtx);
                    writeBlockState->enableUserIndexBuildBlocking(opCtx);
                    // Ensure that we eventually restore index build state.
                    ScopeGuard guard(
                        [&]() { writeBlockState->disableUserIndexBuildBlocking(opCtx); });
                    // Abort and wait for ongoing index builds to finish.
                    IndexBuildsCoordinator::get(opCtx)->abortIndexBuildsForWriteBlocking(opCtx);

                    // Engage write blocking
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->acquireRecoverableCriticalSectionBlockingUserWrites(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace,
                            request().getReason());
                } else {
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->releaseRecoverableCriticalSectionBlockingUserWrites(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace,
                            request().getReason());
                }
            }

            // Wait for the writes to the UserWritesRecoverableCriticalSection collection to be
            // majority commited.
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
                            ActionType::setUserWriteBlockMode}));
        }

        std::mutex _mutex;
    };
};
MONGO_REGISTER_COMMAND(SetUserWriteBlockModeCommand).forShard();
}  // namespace
}  // namespace mongo
