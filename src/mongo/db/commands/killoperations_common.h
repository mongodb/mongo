// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/kill_operations_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_killer.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

template <typename Derived>
class KillOperationsCmdBase : public TypedCommand<Derived> {
public:
    using Request = KillOperationsRequest;

    class Invocation final : public TypedCommand<Derived>::InvocationBase {
    public:
        using Base = typename TypedCommand<Derived>::InvocationBase;
        using Base::Base;

        void typedRun(OperationContext* opCtx) {
            auto opKiller = OperationKiller(opCtx->getClient());
            auto opKeys = Base::request().getOperationKeys();

            for (auto& opKey : opKeys) {
                LOGV2_DEBUG(
                    4615602, 2, "Attempting to kill operation", "operationKey"_attr = opKey);
                opKiller.killOperation(OperationKey(opKey));
            }
            Derived::killCursors(opCtx, opKeys);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(Base::request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto client = opCtx->getClient();
            auto isInternal = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(Base::request().getDbName().tenantId()),
                ActionType::internal);
            if (!getTestCommandsEnabled() && !isInternal) {
                // Either the mongod/mongos must be in testing mode or this command must come from
                // an internal user
                uasserted(ErrorCodes::IllegalOperation,
                          "_killOperations is only to be used by internal users");
            }

            auto opKiller = OperationKiller(client);
            if (!opKiller.isGenerallyAuthorizedToKill()) {
                // While it's feasible to have coauthorized killers, for now this is intended for
                // internal use, so general kill auth is implied.
                uasserted(ErrorCodes::Unauthorized, "Unauthorized");
            }
        }
    };

private:
    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command -- Kill operations on the target server by OperationKey.";
    }
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
