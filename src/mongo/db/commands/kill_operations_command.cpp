/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/kill_operations_gen.h"

#include <fmt/format.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_killer.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {

class KillOperationsCommand final : public TypedCommand<KillOperationsCommand> {
public:
    using Request = KillOperationsRequest;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto opKiller = OperationKiller(opCtx->getClient());
            for (auto& opKey : request().getOperationKeys()) {
                LOGV2(4615601,
                      "Attempting to kill operation with OperationKey '{operationKey}'",
                      "operationKey"_attr = opKey);
                opKiller.killOperation(OperationKey(opKey));
            }
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto client = opCtx->getClient();
            auto isInternal = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal);
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
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command -- Kill operations on the target server by OperationKey.";
    }
} killOperationsCmd;

}  // namespace mongo
