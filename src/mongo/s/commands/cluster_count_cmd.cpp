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
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/cluster_aggregate.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

class ClusterCountCmd : public ErrmsgCommandDeprecated {
public:
    ClusterCountCmd() : ErrmsgCommandDeprecated("count") {}

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

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
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

        auto countCmdObj = countCmdBuilder.done();

        BSONObj viewDefinition;
        auto swShardResponses =
            scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                       dbname,
                                                       nss,
                                                       countCmdObj,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       Shard::RetryPolicy::kIdempotent,
                                                       filter,
                                                       collation,
                                                       &viewDefinition);

        if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == swShardResponses.getStatus()) {
            if (viewDefinition.isEmpty()) {
                return appendCommandStatus(
                    result,
                    {ErrorCodes::InternalError,
                     str::stream()
                         << "Missing resolved view definition, but remote returned "
                         << ErrorCodes::errorString(swShardResponses.getStatus().code())});
            }

            // Rewrite the count command as an aggregation.

            auto countRequest = CountRequest::parseFromBSON(nss, cmdObj, false);
            if (!countRequest.isOK()) {
                return appendCommandStatus(result, countRequest.getStatus());
            }

            auto aggCmdOnView = countRequest.getValue().asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return appendCommandStatus(result, aggCmdOnView.getStatus());
            }

            auto aggRequestOnView = AggregationRequest::parseFromBSON(nss, aggCmdOnView.getValue());
            if (!aggRequestOnView.isOK()) {
                return appendCommandStatus(result, aggRequestOnView.getStatus());
            }

            auto resolvedView = ResolvedView::fromBSON(viewDefinition);
            auto resolvedAggRequest =
                resolvedView.asExpandedViewAggregation(aggRequestOnView.getValue());
            auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();

            BSONObj aggResult = Command::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(resolvedAggCmd)));

            result.resetToEmpty();
            ViewResponseFormatter formatter(aggResult);
            auto formatStatus = formatter.appendAsCountResponse(&result);
            if (!formatStatus.isOK()) {
                return appendCommandStatus(result, formatStatus);
            }

            return true;
        }

        std::vector<AsyncRequestsSender::Response> shardResponses;
        if (ErrorCodes::NamespaceNotFound == swShardResponses.getStatus().code()) {
            // If there's no collection with this name, the count aggregation behavior below
            // will produce a total count of 0.
            shardResponses = {};
        } else {
            uassertStatusOK(swShardResponses.getStatus());
            shardResponses = std::move(swShardResponses.getValue());
        }

        long long total = 0;
        BSONObjBuilder shardSubTotal(result.subobjStart("shards"));

        for (const auto& response : shardResponses) {
            auto status = response.swResponse.getStatus();
            if (status.isOK()) {
                status = getStatusFromCommandResult(response.swResponse.getValue().data);
                if (status.isOK()) {
                    long long shardCount = response.swResponse.getValue().data["n"].numberLong();
                    shardSubTotal.appendNumber(response.shardId.toString(), shardCount);
                    total += shardCount;
                    continue;
                }
            }

            shardSubTotal.doneFast();
            // Add error context so that you can see on which shard failed as well as details
            // about that error.
            auto errorWithContext = Status(status.code(),
                                           str::stream() << "failed on: " << response.shardId
                                                         << causedBy(status.reason()));
            return appendCommandStatus(result, errorWithContext);
        }

        shardSubTotal.doneFast();
        total = applySkipLimit(total, cmdObj);
        result.appendNumber("n", total);
        return true;
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
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

        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // We will time how long it takes to run the commands on the shards
        Timer timer;

        BSONObj viewDefinition;
        auto swShardResponses =
            scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                       dbname,
                                                       nss,
                                                       explainCmd,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       Shard::RetryPolicy::kIdempotent,
                                                       targetingQuery,
                                                       targetingCollation,
                                                       &viewDefinition);

        long long millisElapsed = timer.millis();

        if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == swShardResponses.getStatus()) {
            uassert(ErrorCodes::InternalError,
                    str::stream() << "Missing resolved view definition, but remote returned "
                                  << ErrorCodes::errorString(swShardResponses.getStatus().code()),
                    !viewDefinition.isEmpty());

            auto countRequest = CountRequest::parseFromBSON(nss, cmdObj, true);
            if (!countRequest.isOK()) {
                return countRequest.getStatus();
            }

            auto aggCmdOnView = countRequest.getValue().asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return aggCmdOnView.getStatus();
            }

            auto aggRequestOnView =
                AggregationRequest::parseFromBSON(nss, aggCmdOnView.getValue(), verbosity);
            if (!aggRequestOnView.isOK()) {
                return aggRequestOnView.getStatus();
            }

            auto resolvedView = ResolvedView::fromBSON(viewDefinition);
            auto resolvedAggRequest =
                resolvedView.asExpandedViewAggregation(aggRequestOnView.getValue());
            auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();

            ClusterAggregate::Namespaces nsStruct;
            nsStruct.requestedNss = nss;
            nsStruct.executionNss = resolvedAggRequest.getNamespaceString();

            return ClusterAggregate::runAggregate(
                opCtx, nsStruct, resolvedAggRequest, resolvedAggCmd, out);
        }

        uassertStatusOK(swShardResponses.getStatus());
        auto shardResponses = std::move(swShardResponses.getValue());

        const char* mongosStageName =
            ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

        return ClusterExplain::buildExplainResult(
            opCtx,
            ClusterExplain::downconvert(opCtx, shardResponses),
            mongosStageName,
            millisElapsed,
            out);
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
