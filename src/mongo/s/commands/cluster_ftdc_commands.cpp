// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <string>

namespace mongo {
namespace {

/**
 * getDiagnosticData is a MongoD only command. We implement in MongoS to give users a better error
 * message.
 */
class GetDiagnosticDataCommand final : public ErrmsgCommandDeprecated {
public:
    GetDiagnosticDataCommand() : ErrmsgCommandDeprecated("getDiagnosticData") {}

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
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());

        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                {ActionType::serverStatus,
                 ActionType::replSetGetStatus,
                 ActionType::connPoolStats})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString::kRsOplogNamespace),
                ActionType::collStats)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {

        result.append("data", getMostRecentFTDCDocument(opCtx->getServiceContext()));

        return true;
    }
};
MONGO_REGISTER_COMMAND(GetDiagnosticDataCommand).forRouter();

}  // namespace
}  // namespace mongo
