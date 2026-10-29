// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <string>

namespace mongo {
namespace {

class CmdReplSetGetStatus : public ErrmsgCommandDeprecated {
public:
    CmdReplSetGetStatus() : ErrmsgCommandDeprecated("replSetGetStatus") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }


    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Not supported through mongos";
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        // Require no auth since this command isn't supported in mongos
        return Status::OK();
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        if (cmdObj["forShell"].trueValue()) {
            NotPrimaryErrorTracker::get(cc()).disable();
        }

        errmsg = "replSetGetStatus is not supported through mongos";
        result.append("info", "mongos");

        return false;
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetGetStatus).forRouter();

}  // namespace
}  // namespace mongo
