// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/isself.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"

#include <iosfwd>
#include <string>

namespace mongo {

using std::string;
using std::stringstream;

class IsSelfCommand : public BasicCommand {
public:
    IsSelfCommand() : BasicCommand("_isSelf") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "{ _isSelf : 1 } INTERNAL ONLY";
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

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Critical to observability and diagnosability, annotate as immediate priority.
        ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
            opCtx, AdmissionContext::Priority::kExempt);
        result.append("id", repl::instanceId);
        return true;
    }
};

MONGO_REGISTER_COMMAND(IsSelfCommand).forRouter().forShard();

}  // namespace mongo
