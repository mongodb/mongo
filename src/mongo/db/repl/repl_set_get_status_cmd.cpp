// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/database_name.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {
namespace repl {

class CmdReplSetGetStatus : public ReplSetCommand {
public:
    std::string help() const override {
        return "Report status of a replica set from the POV of this server\n"
               "{ replSetGetStatus : 1 }\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }

    CmdReplSetGetStatus() : ReplSetCommand("replSetGetStatus") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Critical to monitoring and observability, categorize the command as immediate priority.
        ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
            opCtx, AdmissionContext::Priority::kExempt);

        if (cmdObj["forShell"].trueValue())
            NotPrimaryErrorTracker::get(opCtx->getClient()).disable();

        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        // The initialSync parameter accepts:
        //   0 or false: exclude initial sync data (kBasic)
        //   1 or true:  include full initial sync data (kInitialSync)
        //   2:          include summary initial sync data without per-collection detail
        //               (kInitialSyncSummary)
        auto responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kInitialSync;
        if (auto initialSyncElem = cmdObj["initialSync"]) {
            if (initialSyncElem.isNumber()) {
                auto val = initialSyncElem.safeNumberInt();
                switch (val) {
                    case 0:
                        responseStyle =
                            ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic;
                        break;
                    case 1:
                        responseStyle =
                            ReplicationCoordinator::ReplSetGetStatusResponseStyle::kInitialSync;
                        break;
                    case 2:
                        responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::
                            kInitialSyncSummary;
                        break;
                    default:
                        uasserted(ErrorCodes::BadValue,
                                  str::stream()
                                      << "initialSync must be 0, 1, or 2, but received: " << val);
                }
            } else if (initialSyncElem.type() == BSONType::boolean) {
                if (!initialSyncElem.boolean()) {
                    responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic;
                }
            } else {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "initialSync must be a boolean or integer, but received: "
                              << typeName(initialSyncElem.type()));
            }
        }
        status = ReplicationCoordinator::get(opCtx)->processReplSetGetStatus(
            opCtx, &result, responseStyle);
        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetStatus};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetGetStatus).forShard();

}  // namespace repl
}  // namespace mongo
