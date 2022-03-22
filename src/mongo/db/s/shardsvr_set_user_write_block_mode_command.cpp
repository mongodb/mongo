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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/fail_point.h"

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
                    serverGlobalParams.clusterRole == ClusterRole::ShardServer);
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            hangInShardsvrSetUserWriteBlockMode.pauseWhileSet();

            const auto startBlocking = request().getGlobal();

            if (startBlocking) {
                switch (request().getPhase()) {
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare:
                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->acquireRecoverableCriticalSectionBlockNewShardedDDL(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);

                        // Wait for ongoing ShardingDDLCoordinators to finish. This ensures that all
                        // coordinators that started before enabling blocking have finish, and that
                        // any new coordinator that is started after this point will see the
                        // blocking is enabled.
                        ShardingDDLCoordinatorService::getService(opCtx)
                            ->waitForOngoingCoordinatorsToFinish(opCtx);
                        break;
                    case ShardsvrSetUserWriteBlockModePhaseEnum::kComplete:
                        UserWritesRecoverableCriticalSectionService::get(opCtx)
                            ->promoteRecoverableCriticalSectionToBlockUserWrites(
                                opCtx,
                                UserWritesRecoverableCriticalSectionService::
                                    kGlobalUserWritesNamespace);
                        break;
                    default:
                        MONGO_UNREACHABLE;
                }
            } else {
                switch (request().getPhase()) {
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

    private:
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
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
} shardsvrSetUserWriteBlockModeCmd;

}  // namespace
}  // namespace mongo
