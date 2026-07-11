// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

class CmdReplSetRequestVotes : public ReplSetCommand {
public:
    CmdReplSetRequestVotes() : ReplSetCommand("replSetRequestVotes") {}

private:
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        ReplSetRequestVotesArgs parsedArgs;
        status = parsedArgs.initialize(cmdObj);
        uassertStatusOK(status);

        // Operations that are part of Replica Set elections are crucial to the stability of the
        // cluster. Marking it as having Immediate priority will make it skip waiting for ticket
        // acquisition and Flow Control.
        ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
            opCtx, AdmissionContext::Priority::kExempt);
        ReplSetRequestVotesResponse response;
        status = ReplicationCoordinator::get(opCtx)->processReplSetRequestVotes(
            opCtx, parsedArgs, &response);
        uassertStatusOK(status);

        response.addToBSON(&result);
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetRequestVotes).forShard();

}  // namespace repl
}  // namespace mongo
