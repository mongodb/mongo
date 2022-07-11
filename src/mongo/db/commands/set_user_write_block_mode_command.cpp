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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_user_write_block_mode_gen.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/logv2/log.h"

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
                    str::stream() << Request::kCommandName
                                  << " cannot be run on shardsvrs nor configsvrs",
                    serverGlobalParams.clusterRole == ClusterRole::None);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " cannot be run on standalones",
                    repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
                        repl::ReplicationCoordinator::modeNone);

            // Only one attempt to change write block mode may make progress at once, because the
            // way we enable/disable user index build blocking is not concurrency-safe.
            stdx::lock_guard lock(_mutex);
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
                    IndexBuildsCoordinator::get(opCtx)->abortUserIndexBuildsForUserWriteBlocking(
                        opCtx);

                    // Engage write blocking
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->acquireRecoverableCriticalSectionBlockingUserWrites(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                } else {
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->releaseRecoverableCriticalSection(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                }
            }

            // Wait for the writes to the UserWritesRecoverableCriticalSection collection to be
            // majority commited.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            WriteConcernResult writeConcernResult;
            WriteConcernOptions majority(WriteConcernOptions::kMajority,
                                         WriteConcernOptions::SyncMode::UNSET,
                                         WriteConcernOptions::kWriteConcernTimeoutUserCommand);
            uassertStatusOK(
                waitForWriteConcern(opCtx, replClient.getLastOp(), majority, &writeConcernResult));
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const override {
            return NamespaceString();
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                             ActionType::setUserWriteBlockMode}));
        }

        Mutex _mutex = MONGO_MAKE_LATCH("SetUserWriteBlockModeCommand::_mutex");
    };
} setUserWriteBlockModeCommand;
}  // namespace
}  // namespace mongo
