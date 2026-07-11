// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <set>
#include <string>

namespace mongo {

/**
 * Command for testing API Version deprecation logic. The command replies with the values of the
 * OperationContext's API parameters.
 */
class TestDeprecationCmd : public BasicCommand {
public:
    TestDeprecationCmd() : BasicCommand("testDeprecation") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    const std::set<std::string>& deprecatedApiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool requiresAuth() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    std::string help() const override {
        return "replies with the values of the OperationContext's API parameters";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        APIParameters::get(opCtx).appendInfo(&result);
        return true;
    }
};

MONGO_REGISTER_COMMAND(TestDeprecationCmd).testOnly().forRouter().forShard();


}  // namespace mongo
