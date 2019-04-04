/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/db/client.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/repl/optime.h"
#include "merizo/db/repl/repl_set_command.h"
#include "merizo/db/repl/repl_set_request_votes_args.h"
#include "merizo/db/repl/replication_coordinator.h"
#include "merizo/executor/network_interface.h"
#include "merizo/transport/session.h"
#include "merizo/transport/transport_layer.h"
#include "merizo/util/scopeguard.h"

namespace merizo {
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
        uassertStatusOK(status);

        response.addToBSON(&result);
        return true;
    }
} cmdReplSetRequestVotes;

}  // namespace repl
}  // namespace merizo
