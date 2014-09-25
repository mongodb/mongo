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

#include "mongo/s/commands/cluster_explain_cmd.h"

#include "mongo/db/query/explain.h"

namespace mongo {

    static ClusterExplainCmd cmdExplainCluster;

    Status ClusterExplainCmd::checkAuthForCommand(ClientBasic* client,
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

    bool ClusterExplainCmd::run(OperationContext* txn, const string& dbName,
                                BSONObj& cmdObj,
                                int options,
                                string& errmsg,
                                BSONObjBuilder& result,
                                bool fromRepl) {
        // Should never get explain commands issued from replication.
        if (fromRepl) {
            Status commandStat(ErrorCodes::IllegalOperation,
                               "explain command should not be from repl");
            return appendCommandStatus(result, commandStat);
        }

        ExplainCommon::Verbosity verbosity;
        Status parseStatus = ExplainCommon::parseCmdBSON(cmdObj, &verbosity);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus);
        }

        // This is the nested command which we are explaining.
        BSONObj explainObj = cmdObj.firstElement().Obj();

        const std::string cmdName = explainObj.firstElementFieldName();
        Command* commToExplain = Command::findCommand(cmdName);
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "Explain failed due to unknown command: " << cmdName;
            Status explainStatus(ErrorCodes::CommandNotFound, ss);
            return appendCommandStatus(result, explainStatus);
        }

        // Actually call the nested command's explain(...) method.
        Status explainStatus = commToExplain->explain(txn, dbName, explainObj, verbosity, &result);
        if (!explainStatus.isOK()) {
            return appendCommandStatus(result, explainStatus);
        }

        return true;
    }

} // namespace mongo
