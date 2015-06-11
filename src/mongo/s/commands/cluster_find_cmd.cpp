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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/cluster_explain.h"
#include "mongo/s/strategy.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

    using std::unique_ptr;
    using std::string;
    using std::vector;

    /**
     * Implements the find command on mongos.
     *
     * TODO: this is just a placeholder. It needs to be implemented for real under SERVER-15176.
     */
    class ClusterFindCmd : public Command {
        MONGO_DISALLOW_COPYING(ClusterFindCmd);
    public:
        ClusterFindCmd() : Command("find") { }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual bool slaveOk() const { return false; }

        virtual bool slaveOverrideOk() const { return true; }

        virtual bool maintenanceOk() const { return false; }

        virtual bool adminOnly() const { return false; }

        virtual void help(std::stringstream& help) const {
            help << "query for documents";
        }

        /**
         * In order to run the find command, you must be authorized for the "find" action
         * type on the collection.
         */
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {

            AuthorizationSession* authzSession = AuthorizationSession::get(client);
            ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

            if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::find)) {
                return Status::OK();
            }

            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }

        virtual Status explain(OperationContext* txn,
                               const std::string& dbname,
                               const BSONObj& cmdObj,
                               ExplainCommon::Verbosity verbosity,
                               BSONObjBuilder* out) const {

            const string fullns = parseNs(dbname, cmdObj);

            // Parse the command BSON to a LiteParsedQuery.
            bool isExplain = true;
            auto lpqStatus = LiteParsedQuery::fromFindCommand(fullns, cmdObj, isExplain);
            if (!lpqStatus.isOK()) {
                return lpqStatus.getStatus();
            }

            auto& lpq = lpqStatus.getValue();

            BSONObjBuilder explainCmdBob;
            ClusterExplain::wrapAsExplain(cmdObj, verbosity, &explainCmdBob);

            // We will time how long it takes to run the commands on the shards.
            Timer timer;

            vector<Strategy::CommandResult> shardResults;
            Strategy::commandOp(dbname,
                                explainCmdBob.obj(),
                                lpq->getOptions(),
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

        virtual bool run(OperationContext* txn,
                         const std::string& dbname,
                         BSONObj& cmdObj, int options,
                         std::string& errmsg,
                         BSONObjBuilder& result) {

            // Currently only explains of finds run through the find command. Queries that are not
            // explained use the legacy OP_QUERY path.
            errmsg = "find command not yet implemented";
            return false;
        }

    } cmdFindCluster;

} // namespace
} // namespace mongo
