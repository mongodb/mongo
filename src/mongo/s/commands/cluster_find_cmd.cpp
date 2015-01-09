/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/s/commands/cluster_find_cmd.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/s/cluster_explain.h"
#include "mongo/s/strategy.h"
#include "mongo/util/timer.h"

namespace mongo {

    using std::auto_ptr;
    using std::string;
    using std::vector;

    static ClusterFindCmd cmdFindCluster;

    Status ClusterFindCmd::checkAuthForCommand(ClientBasic* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = client->getAuthorizationSession();
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    Status ClusterFindCmd::explain(OperationContext* txn,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj,
                                   ExplainCommon::Verbosity verbosity,
                                   BSONObjBuilder* out) const {
        const string fullns = parseNs(dbname, cmdObj);

        // Parse the command BSON to a LiteParsedQuery.
        LiteParsedQuery* rawLpq;
        bool isExplain = true;
        Status lpqStatus = LiteParsedQuery::make(fullns, cmdObj, isExplain, &rawLpq);
        if (!lpqStatus.isOK()) {
            return lpqStatus;
        }
        auto_ptr<LiteParsedQuery> lpq(rawLpq);

        BSONObjBuilder explainCmdBob;
        ClusterExplain::wrapAsExplain(cmdObj, verbosity, &explainCmdBob);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        vector<Strategy::CommandResult> shardResults;
        STRATEGY->commandOp(dbname,
                            explainCmdBob.obj(),
                            lpq->getOptions().toInt(),
                            fullns,
                            lpq->getFilter(),
                            &shardResults);

        long long millisElapsed = timer.millis();

        const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, cmdObj);

        return ClusterExplain::buildExplainResult(shardResults,
                                                  mongosStageName,
                                                  millisElapsed,
                                                  out);
    }

    bool ClusterFindCmd::run(OperationContext* txn, const string& dbName,
                             BSONObj& cmdObj,
                             int options,
                             string& errmsg,
                             BSONObjBuilder& result,
                              bool fromRepl) {
        // Currently only explains of finds run through the find command. Queries that are not
        // explained use the legacy OP_QUERY path.
        errmsg = "find command not yet implemented";
        return false;
    }

} // namespace mongo
