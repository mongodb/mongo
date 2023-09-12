/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangInShardsvrSetUserWriteBlockMode);

class ShardsvrSetUserWriteBlockCommand final
    : public TypedCommand<ShardsvrSetUserWriteBlockCommand> {
public:
    using Request = ShardsvrSetUserWriteBlockMode;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            hangInShardsvrSetUserWriteBlockMode.pauseWhileSet();

            _runImpl(opCtx, request());

            // Since it is possible that no actual write happened with this txnNumber, we need to
            // make a dummy write so that secondaries can be aware of this txn.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id"
                               << "SetUseWriteBlockModeStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
        }

    private:
        void _runImpl(OperationContext* opCtx, const Request& request) {
            const auto startBlocking = request.getGlobal();

            if (startBlocking) {
                switch (request.getPhase()) {
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare:
                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->acquireRecoverableCriticalSectionBlockNewShardedDDL(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);

                        // Wait for ongoing ShardingDDLCoordinators to finish. This ensures that all
                        // coordinators that started before enabling blocking have finish, and that
                        // any new coordinator that is started after this point will see the
                        // blocking is enabled. Wait only for coordinators that don't have the
                        // user-write-blocking bypass enabled -- the ones allowed to bypass user
                        // write blocking don't care about the write blocking state.
                        {
                            const auto shouldWaitPred =
                                [](const ShardingDDLCoordinator& coordinatorInstance) -> bool {
                                // No need to wait for coordinators that do not modify user data.
                                if (coordinatorInstance.canAlwaysStartWhenUserWritesAreDisabled()) {
                                    return false;
                                }

                                // Don't wait for coordinator instances that are allowed to bypass
                                // user write blocking.
                                if (coordinatorInstance.getForwardableOpMetadata()
                                        .getMayBypassWriteBlocking()) {
                                    return false;
                                }

                                return true;
                            };

                            ShardingDDLCoordinatorService::getService(opCtx)
                                ->waitForOngoingCoordinatorsToFinish(opCtx, shouldWaitPred);
                        }
                        break;
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kComplete: {
                        // The way we enable/disable user index build blocking is not
                        // concurrency-safe, so use a mutex to make this a critical section
                        stdx::lock_guard lock(_mutex);
                        auto writeBlockState = GlobalUserWriteBlockState::get(opCtx);
                        writeBlockState->enableUserIndexBuildBlocking(opCtx);
                        // Ensure that we eventually restore index build state.
                        ScopeGuard guard(
                            [&]() { writeBlockState->disableUserIndexBuildBlocking(opCtx); });
                        // Abort and wait for ongoing index builds to finish.
                        IndexBuildsCoordinator::get(opCtx)
                            ->abortUserIndexBuildsForUserWriteBlocking(opCtx);

                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->promoteRecoverableCriticalSectionToBlockUserWrites(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);
                    } break;
                    default:
                        MONGO_UNREACHABLE;
                }
            } else {
                switch (request.getPhase()) {
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare:
                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->demoteRecoverableCriticalSectionToNoLongerBlockUserWrites(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);
                        break;
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kComplete:
                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->releaseRecoverableCriticalSection(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);
                        break;
                    default:
                        MONGO_UNREACHABLE;
                }
            }
        }

        NamespaceString ns() const override {
            return NamespaceString();
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

        Mutex _mutex = MONGO_MAKE_LATCH("ShardsvrSetUserWriteBlockCommand::_mutex");
    };

    std::string help() const override {
        return "Internal command, which is exported by the shard servers. Do not call "
               "directly. Enables/disables user write blocking on shardsvrs.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrSetUserWriteBlockCommand).forShard();

}  // namespace
}  // namespace mongo
