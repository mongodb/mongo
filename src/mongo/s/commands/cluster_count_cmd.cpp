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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/count_request.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

class ClusterCountCmd : public Command {
public:
    ClusterCountCmd() : Command("count", false) {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
                nss.isValid());

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

        BSONObj collation;
        BSONElement collationElement;
        auto status =
            bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            collation = collationElement.Obj();
        } else if (status != ErrorCodes::NoSuchKey) {
            return appendCommandStatus(result, status);
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

        std::vector<Strategy::CommandResult> countResult;
        Strategy::commandOp(opCtx,
                            dbname,
                            countCmdBuilder.done(),
                            options,
                            nss.ns(),
                            filter,
                            collation,
                            &countResult);

        if (countResult.size() == 1 &&
            ResolvedView::isResolvedViewErrorResponse(countResult[0].result)) {
            auto countRequest = CountRequest::parseFromBSON(dbname, cmdObj, false);
            if (!countRequest.isOK()) {
                return appendCommandStatus(result, countRequest.getStatus());
            }

            auto aggCmdOnView = countRequest.getValue().asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return appendCommandStatus(result, aggCmdOnView.getStatus());
            }

            auto resolvedView = ResolvedView::fromBSON(countResult[0].result);
            auto aggCmd = resolvedView.asExpandedViewAggregation(aggCmdOnView.getValue());
            if (!aggCmd.isOK()) {
                return appendCommandStatus(result, aggCmd.getStatus());
            }


            BSONObjBuilder aggResult;
            Command::findCommand("aggregate")
                ->run(opCtx, dbname, aggCmd.getValue(), options, errmsg, aggResult);

            result.resetToEmpty();
            ViewResponseFormatter formatter(aggResult.obj());
            auto formatStatus = formatter.appendAsCountResponse(&result);
            if (!formatStatus.isOK()) {
                return appendCommandStatus(result, formatStatus);
            }

            return true;
        }


        long long total = 0;
        BSONObjBuilder shardSubTotal(result.subobjStart("shards"));

        for (const auto& resultEntry : countResult) {
            const ShardId& shardName = resultEntry.shardTargetId;
            const auto resultBSON = resultEntry.result;

            if (resultBSON["ok"].trueValue()) {
                long long shardCount = resultBSON["n"].numberLong();

                shardSubTotal.appendNumber(shardName.toString(), shardCount);
                total += shardCount;
            } else {
                shardSubTotal.doneFast();
                errmsg = "failed on : " + shardName.toString();
                result.append("cause", resultBSON);

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

    Status explain(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                   BSONObjBuilder* out) const override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
                nss.isValid());

        // Extract the targeting query.
        BSONObj targetingQuery;
        if (Object == cmdObj["query"].type()) {
            targetingQuery = cmdObj["query"].Obj();
        }

        // Extract the targeting collation.
        BSONObj targetingCollation;
        BSONElement targetingCollationElement;
        auto status = bsonExtractTypedField(
            cmdObj, "collation", BSONType::Object, &targetingCollationElement);
        if (status.isOK()) {
            targetingCollation = targetingCollationElement.Obj();
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }

        BSONObjBuilder explainCmdBob;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

        // We will time how long it takes to run the commands on the shards
        Timer timer;

        std::vector<Strategy::CommandResult> shardResults;
        Strategy::commandOp(opCtx,
                            dbname,
                            explainCmdBob.obj(),
                            options,
                            nss.ns(),
                            targetingQuery,
                            targetingCollation,
                            &shardResults);

        long long millisElapsed = timer.millis();

        if (shardResults.size() == 1 &&
            ResolvedView::isResolvedViewErrorResponse(shardResults[0].result)) {
            auto countRequest = CountRequest::parseFromBSON(dbname, cmdObj, true);
            if (!countRequest.isOK()) {
                return countRequest.getStatus();
            }

            auto aggCmdOnView = countRequest.getValue().asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return aggCmdOnView.getStatus();
            }

            auto resolvedView = ResolvedView::fromBSON(shardResults[0].result);
            auto aggCmd = resolvedView.asExpandedViewAggregation(aggCmdOnView.getValue());
            if (!aggCmd.isOK()) {
                return aggCmd.getStatus();
            }

            std::string errMsg;
            if (Command::findCommand("aggregate")
                    ->run(opCtx, dbname, aggCmd.getValue(), 0, errMsg, *out)) {
                return Status::OK();
            }

            return getStatusFromCommandResult(out->asTempObj());
        }

        const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, cmdObj);

        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, mongosStageName, millisElapsed, out);
    }

private:
    static long long applySkipLimit(long long num, const BSONObj& cmd) {
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

} clusterCountCmd;

}  // namespace
}  // namespace mongo
