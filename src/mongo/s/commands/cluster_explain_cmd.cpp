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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/explain.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/query/cluster_find.h"

namespace mongo {
namespace {

/**
 * Implements the explain command on mongos.
 *
 * "Old-style" explains (i.e. queries which have the $explain flag set), do not run
 * through this path. Such explains will be supported for backwards compatibility,
 * and must succeed in multiversion clusters.
 *
 * "New-style" explains use the explain command. When the explain command is routed
 * through mongos, it is forwarded to all relevant shards. If *any* shard does not
 * support a new-style explain, then the entire explain will fail (i.e. new-style
 * explains cannot be used in multiversion clusters).
 */
class ClusterExplainCmd : public Command {
    MONGO_DISALLOW_COPYING(ClusterExplainCmd);

public:
    ClusterExplainCmd() : Command("explain") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    /**
     * Running an explain on a secondary requires explicitly setting slaveOk.
     */
    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool maintenanceOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "explain database reads and writes";
    }

    /**
     * You are authorized to run an explain if you are authorized to run
     * the command that you are explaining. The auth check is performed recursively
     * on the nested command.
     */
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (Object != cmdObj.firstElement().type()) {
            return Status(ErrorCodes::BadValue, "explain command requires a nested object");
        }

        BSONObj explainObj = cmdObj.firstElement().Obj();

        Command* commToExplain = Command::findCommand(explainObj.firstElementFieldName());
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "unknown command: " << explainObj.firstElementFieldName();
            return Status(ErrorCodes::CommandNotFound, ss);
        }

        return commToExplain->checkAuthForCommand(client, dbname, explainObj);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbName,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        ExplainCommon::Verbosity verbosity;
        Status parseStatus = ExplainCommon::parseCmdBSON(cmdObj, &verbosity);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus);
        }

        // This is the nested command which we are explaining.
        BSONObj explainObj = cmdObj.firstElement().Obj();

        const std::string cmdName = explainObj.firstElementFieldName();
        Command* commToExplain = Command::findCommand(cmdName);
        if (!commToExplain) {
            return appendCommandStatus(
                result,
                Status{ErrorCodes::CommandNotFound,
                       str::stream() << "Explain failed due to unknown command: " << cmdName});
        }

        auto readPref =
            ClusterFind::extractUnwrappedReadPref(cmdObj, options & QueryOption_SlaveOk);
        if (!readPref.isOK()) {
            return appendCommandStatus(result, readPref.getStatus());
        }
        const bool secondaryOk = (readPref.getValue().pref != ReadPreference::PrimaryOnly);
        rpc::ServerSelectionMetadata metadata(secondaryOk, readPref.getValue());

        // Actually call the nested command's explain(...) method.
        Status explainStatus =
            commToExplain->explain(txn, dbName, explainObj, verbosity, metadata, &result);
        if (!explainStatus.isOK()) {
            return appendCommandStatus(result, explainStatus);
        }

        return true;
    }

} cmdExplainCluster;

}  // namespace
}  // namespace mongo
