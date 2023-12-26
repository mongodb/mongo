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

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/database_name.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

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
        ScopedAdmissionPriorityForLock skipAdmissionControl(shard_role_details::getLocker(opCtx),
                                                            AdmissionContext::Priority::kImmediate);

        if (cmdObj["forShell"].trueValue())
            NotPrimaryErrorTracker::get(opCtx->getClient()).disable();

        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        bool includeInitialSync = true;
        Status initialSyncStatus =
            bsonExtractBooleanFieldWithDefault(cmdObj, "initialSync", true, &includeInitialSync);
        uassertStatusOK(initialSyncStatus);

        auto responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic;
        if (includeInitialSync) {
            responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kInitialSync;
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
