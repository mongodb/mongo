/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <string>

namespace mongo {
namespace {

/**
 * Get the most recent document FTDC collected from its periodic collectors.
 *
 * Document will be empty if FTDC has never run.
 */
class GetDiagnosticDataCommand final : public BasicCommand {
public:
    GetDiagnosticDataCommand() : BasicCommand("getDiagnosticData") {}

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "get latest diagnostic data collection snapshot";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* client = opCtx->getClient();

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                {ActionType::serverStatus, ActionType::replSetGetStatus})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString::kRsOplogNamespace),
                ActionType::collStats)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        result.append(
            "data",
            FTDCController::get(opCtx->getServiceContext())->getMostRecentPeriodicDocument());

        return true;
    }
};
MONGO_REGISTER_COMMAND(GetDiagnosticDataCommand).forShard();

/**
 * Triggers a rotate of the FTDC file
 */
class TriggerRotateFTDCCmd : public TypedCommand<TriggerRotateFTDCCmd> {
public:
    using Request = TriggerRotateFTDC;

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            FTDCController::get(opCtx->getServiceContext())->triggerRotate();
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* client = opCtx->getClient();

            auto checkAuth = [&](auto&& resource, auto&&... actions) {
                auto ok = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                    resource, {actions...});
                uassert(ErrorCodes::Unauthorized, "Unauthorized", ok);
            };
            checkAuth(ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                      ActionType::serverStatus,
                      ActionType::replSetGetStatus);
            checkAuth(ResourcePattern::forExactNamespace(NamespaceString::kRsOplogNamespace),
                      ActionType::collStats);
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(TriggerRotateFTDCCmd).forRouter().forShard();

}  // namespace

}  // namespace mongo
