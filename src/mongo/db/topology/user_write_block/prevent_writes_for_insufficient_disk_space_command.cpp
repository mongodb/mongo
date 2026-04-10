/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/user_write_block/prevent_writes_for_insufficient_disk_space_gen.h"
#include "mongo/db/topology/user_write_block/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/write_concern.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangInPreventWritesForInsufficientDiskSpaceCommand);

class PreventWritesForInsufficientDiskSpaceCommand final
    : public TypedCommand<PreventWritesForInsufficientDiskSpaceCommand> {
public:
    using Request = PreventWritesForInsufficientDiskSpace;

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
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " cannot be run on standalones",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());

            // Only one attempt to change whether writes are blocked may make progress at once
            stdx::lock_guard lock(_mutex);
            {
                if (MONGO_unlikely(
                        hangInPreventWritesForInsufficientDiskSpaceCommand.shouldFail())) {
                    LOGV2(12096400,
                          "hangInPreventWritesForInsufficientDiskSpaceCommand failpoint enabled");
                    hangInPreventWritesForInsufficientDiskSpaceCommand.pauseWhileSet(opCtx);
                }

                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << Request::kCommandName
                                      << " cannot be run with allowDeletions option set to true.",
                        !request().getAllowDeletions());
            }
            if (request().getEnabled()) {
                // Enable write blocking
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->acquireRecoverableCriticalSectionPreventingWrites(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::
                            kPreventWritesForInsufficientDiskSpaceNamespace,
                        request().getAllowDeletions(),
                        request().getReason());

            } else {
                // Disable write blocking
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->releaseRecoverableCriticalSectionPreventingWrites(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::
                            kPreventWritesForInsufficientDiskSpaceNamespace,
                        request().getReason());
            }
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
                            ActionType::preventWritesForInsufficientDiskSpace}));
        }

        stdx::mutex _mutex;
    };
};
MONGO_REGISTER_COMMAND(PreventWritesForInsufficientDiskSpaceCommand)
    .requiresFeatureFlag(feature_flags::gFeatureFlagPreventWritesForInsufficientDiskSpace)
    .forShard();
}  // namespace
}  // namespace mongo
