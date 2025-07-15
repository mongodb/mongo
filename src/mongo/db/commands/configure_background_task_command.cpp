/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/commands/configure_background_task_command_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {
class ConfigureBackgroundTaskCommand final : public TypedCommand<ConfigureBackgroundTaskCommand> {
public:
    using Request = ConfigureBackgroundTask;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Configures background tasks with enabled, disabled or throttled mode.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            if (request().getMode() == ConfigureBackgroundTaskModeEnum::kThrottled) {
                uassert(ErrorCodes::IllegalOperation,
                        "Throttle delay can only be set in throttled mode",
                        request().getThrottleDelayMs().has_value());

                uassert(ErrorCodes::IllegalOperation,
                        "Throttle delay can only be set in throttled mode",
                        request().getThrottleDelayMs().get() >= 0);
            }

            if (request().getTask() == ConfigureBackgroundTaskControlEnum::kMigrations) {
                uassert(ErrorCodes::IllegalOperation,
                        "Migrations task can't be set in throttled mode",
                        request().getMode() != ConfigureBackgroundTaskModeEnum::kThrottled);
            }

            if (request().getTask() == ConfigureBackgroundTaskControlEnum::kRangeDeleter ||
                request().getTask() == ConfigureBackgroundTaskControlEnum::kMigrations) {
                uassert(ErrorCodes::IllegalOperation,
                        "The range deleter and migrations task can not be configured in a "
                        "non-sharded toplogy",
                        ShardingState::get(opCtx)->enabled());
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::configureBackgroundTask));
        }
    };
};
MONGO_REGISTER_COMMAND(ConfigureBackgroundTaskCommand).forShard();

}  // namespace
}  // namespace mongo
