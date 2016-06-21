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

#include <vector>

#include "mongo/db/commands.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

long long applySkipLimit(long long num, const BSONObj& cmd) {
    BSONElement s = cmd["skip"];
    BSONElement l = cmd["limit"];

    if (s.isNumber()) {
        num = num - s.numberLong();
        if (num < 0) {
            num = 0;
        }
    }

    if (l.isNumber()) {
        long long limit = l.numberLong();
        if (limit < 0) {
            limit = -limit;
        }

        // 0 limit means no limit
        if (limit < num && limit != 0) {
            num = limit;
        }
    }

    return num;
}


class ClusterCountCmd : public Command {
public:
    ClusterCountCmd() : Command("count", false) {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(
            ErrorCodes::InvalidNamespace, "count command requires valid namespace", nss.isValid());

        long long skip = 0;

        if (cmdObj["skip"].isNumber()) {
            skip = cmdObj["skip"].numberLong();
            if (skip < 0) {
                errmsg = "skip value is negative in count query";
                return false;
            }
        } else if (cmdObj["skip"].ok()) {
            errmsg = "skip value is not a valid number";
            return false;
        }

        BSONObjBuilder countCmdBuilder;
        countCmdBuilder.append("count", nss.coll());

        BSONObj filter;
        if (cmdObj["query"].isABSONObj()) {
            countCmdBuilder.append("query", cmdObj["query"].Obj());
            filter = cmdObj["query"].Obj();
        }

        if (cmdObj["limit"].isNumber()) {
            long long limit = cmdObj["limit"].numberLong();

            // We only need to factor in the skip value when sending to the shards if we
            // have a value for limit, otherwise, we apply it only once we have collected all
            // counts.
            if (limit != 0 && cmdObj["skip"].isNumber()) {
                if (limit > 0)
                    limit += skip;
                else
                    limit -= skip;
            }

            countCmdBuilder.append("limit", limit);
        }

        const std::initializer_list<StringData> passthroughFields = {
            "$queryOptions", "collation", "hint", "readConcern", QueryRequest::cmdOptionMaxTimeMS,
        };
        for (auto name : passthroughFields) {
            if (auto field = cmdObj[name]) {
                countCmdBuilder.append(field);
            }
        }

        vector<Strategy::CommandResult> countResult;
        Strategy::commandOp(
            txn, dbname, countCmdBuilder.done(), options, nss.ns(), filter, &countResult);

        long long total = 0;
        BSONObjBuilder shardSubTotal(result.subobjStart("shards"));

        for (vector<Strategy::CommandResult>::const_iterator iter = countResult.begin();
             iter != countResult.end();
             ++iter) {
            const ShardId& shardName = iter->shardTargetId;

            if (iter->result["ok"].trueValue()) {
                long long shardCount = iter->result["n"].numberLong();

                shardSubTotal.appendNumber(shardName.toString(), shardCount);
                total += shardCount;
            } else {
                shardSubTotal.doneFast();
                errmsg = "failed on : " + shardName.toString();
                result.append("cause", iter->result);

                // Add "code" to the top-level response, if the failure of the sharded command
                // can be accounted to a single error
                int code = getUniqueCodeFromCommandResults(countResult);
                if (code != 0) {
                    result.append("code", code);
                }

                return false;
            }
        }

        shardSubTotal.doneFast();
        total = applySkipLimit(total, cmdObj);
        result.appendNumber("n", total);

        return true;
    }

    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                           BSONObjBuilder* out) const {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        if (!nss.isValid()) {
            return Status{ErrorCodes::InvalidNamespace,
                          str::stream() << "Invalid collection name: " << nss.ns()};
        }

        // Extract the targeting query.
        BSONObj targetingQuery;
        if (Object == cmdObj["query"].type()) {
            targetingQuery = cmdObj["query"].Obj();
        }

        BSONObjBuilder explainCmdBob;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

        // We will time how long it takes to run the commands on the shards
        Timer timer;

        vector<Strategy::CommandResult> shardResults;
        Strategy::commandOp(
            txn, dbname, explainCmdBob.obj(), options, nss.ns(), targetingQuery, &shardResults);

        long long millisElapsed = timer.millis();

        const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, cmdObj);

        return ClusterExplain::buildExplainResult(
            txn, shardResults, mongosStageName, millisElapsed, out);
    }

} clusterCountCmd;

}  // namespace
}  // namespace mongo
