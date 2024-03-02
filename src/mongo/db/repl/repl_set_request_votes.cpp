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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"

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
        ScopedAdmissionPriority priority(opCtx, AdmissionContext::Priority::kExempt);
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
