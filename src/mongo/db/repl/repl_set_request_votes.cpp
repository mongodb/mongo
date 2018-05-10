/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

class CmdReplSetRequestVotes : public ReplSetCommand {
public:
    CmdReplSetRequestVotes() : ReplSetCommand("replSetRequestVotes") {}

private:
    bool run(OperationContext* opCtx,
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        ReplSetRequestVotesArgs parsedArgs;
        status = parsedArgs.initialize(cmdObj);
        uassertStatusOK(status);

        ReplSetRequestVotesResponse response;
        status = ReplicationCoordinator::get(opCtx)->processReplSetRequestVotes(
            opCtx, parsedArgs, &response);
        response.addToBSON(&result);
        return CommandHelpers::appendCommandStatusNoThrow(result, status);
    }
} cmdReplSetRequestVotes;

}  // namespace repl
}  // namespace mongo
