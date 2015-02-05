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

#include "mongo/base/init.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/strategy.h"

namespace mongo {

    using std::string;
    using std::stringstream;
    using std::vector;

    /**
     * Base class for mongos index filter commands.
     * Cluster index filter commands don't do much more than
     * forwarding the commands to all shards and combining the results.
     */
    class ClusterIndexFilterCmd : public Command {
    MONGO_DISALLOW_COPYING(ClusterIndexFilterCmd);
    public:

        virtual ~ClusterIndexFilterCmd() {
        }

        bool slaveOk() const {
            return false;
        }

        bool slaveOverrideOk() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        void help(stringstream& ss) const {
            ss << _helpText;
        }

        Status checkAuthForCommand( ClientBasic* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj ) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);
    
            if (authzSession->isAuthorizedForActionsOnResource(pattern,
                                                               ActionType::planCacheIndexFilter)) {
                return Status::OK();
            }
    
            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }

        // Cluster plan cache command entry point.
        bool run(OperationContext* txn, const std::string& dbname,
                  BSONObj& cmdObj,
                  int options,
                  std::string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl );

    public:

        /**
         * Instantiates a command that can be invoked by "name", which will be described by
         * "helpText", and will require privilege "actionType" to run.
         */
        ClusterIndexFilterCmd( const std::string& name, const std::string& helpText) :
            Command( name ), _helpText( helpText ) {
        }

    private:

        std::string _helpText;
    };

    //
    // Cluster index filter command implementation(s) below
    //

    bool ClusterIndexFilterCmd::run(OperationContext* txn, const std::string& dbName,
                               BSONObj& cmdObj,
                               int options,
                               std::string& errMsg,
                               BSONObjBuilder& result,
                               bool ) {
        const std::string fullns = parseNs(dbName, cmdObj);
        NamespaceString nss(fullns);

        // Dispatch command to all the shards.
        // Targeted shard commands are generally data-dependent but index filter
        // commands are tied to query shape (data has no effect on query shape).
        vector<Strategy::CommandResult> results;
        STRATEGY->commandOp(dbName, cmdObj, options, nss.ns(), BSONObj(), &results);

        // Set value of first shard result's "ok" field.
        bool clusterCmdResult = true;

        for (vector<Strategy::CommandResult>::const_iterator i = results.begin();
             i != results.end(); ++i) {
            const Strategy::CommandResult& cmdResult = *i;

            // XXX: In absence of sensible aggregation strategy,
            //      promote first shard's result to top level.
            if (i == results.begin()) {
                result.appendElements(cmdResult.result);
                clusterCmdResult = cmdResult.result["ok"].trueValue();
            }

            // Append shard result as a sub object.
            // Name the field after the shard.
            string shardName = cmdResult.shardTarget.getName();
            result.append(shardName, cmdResult.result);
        }

        return clusterCmdResult;
    }

    //
    // Register index filter commands at startup
    //

    namespace {

        MONGO_INITIALIZER(RegisterIndexFilterCommands)(InitializerContext* context) {
            // Leaked intentionally: a Command registers itself when constructed.

            new ClusterIndexFilterCmd(
                "planCacheListFilters",
                "Displays index filters for all query shapes in a collection." );

            new ClusterIndexFilterCmd(
                "planCacheClearFilters",
                "Clears index filter for a single query shape or, "
                "if the query shape is omitted, all filters for the collection." );

            new ClusterIndexFilterCmd(
                "planCacheSetFilter",
                "Sets index filter for a query shape. Overrides existing index filter." );

            return Status::OK();
        }

    } // namespace

} // namespace mongo
