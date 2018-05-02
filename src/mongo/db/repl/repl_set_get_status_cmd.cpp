/**
*    Copyright (C) 2018 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/repl/repl_set_command.h"

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
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        if (cmdObj["forShell"].trueValue())
            LastError::get(opCtx->getClient()).disable();

        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        bool includeInitialSync = false;
        Status initialSyncStatus =
            bsonExtractBooleanFieldWithDefault(cmdObj, "initialSync", false, &includeInitialSync);
        uassertStatusOK(initialSyncStatus);

        auto responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic;
        if (includeInitialSync) {
            responseStyle = ReplicationCoordinator::ReplSetGetStatusResponseStyle::kInitialSync;
        }
        status =
            ReplicationCoordinator::get(opCtx)->processReplSetGetStatus(&result, responseStyle);
        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetStatus};
    }
} cmdReplSetGetStatus;

}  // namespace repl
}  // namespace mongo
