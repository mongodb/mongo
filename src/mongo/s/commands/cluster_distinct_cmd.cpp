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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/cluster_aggregate.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class DistinctCmd : public BasicCommand {
public:
    DistinctCmd() : BasicCommand("distinct") {}

    std::string help() const override {
        return "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& opMsgRequest,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        std::string dbname = opMsgRequest.getDatabase().toString();
        const BSONObj& cmdObj = opMsgRequest.body;
        const NamespaceString nss(parseNs(dbname, cmdObj));

        auto targetingQuery = extractQuery(cmdObj);
        auto targetingCollation = extractCollation(cmdObj);

        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            const auto routingInfo = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            shardResponses =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           nss.db(),
                                                           nss,
                                                           routingInfo,
                                                           explainCmd,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           targetingQuery,
                                                           targetingCollation);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
            auto parsedDistinct = ParsedDistinct::parse(
                opCtx, ex->getNamespace(), cmdObj, ExtensionsCallbackNoop(), true);
            if (!parsedDistinct.isOK()) {
                return parsedDistinct.getStatus();
            }

            auto aggCmdOnView = parsedDistinct.getValue().asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return aggCmdOnView.getStatus();
            }

            auto aggRequestOnView =
                AggregationRequest::parseFromBSON(nss, aggCmdOnView.getValue(), verbosity);
            if (!aggRequestOnView.isOK()) {
                return aggRequestOnView.getStatus();
            }

            auto resolvedAggRequest = ex->asExpandedViewAggregation(aggRequestOnView.getValue());
            auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();

            ClusterAggregate::Namespaces nsStruct;
            nsStruct.requestedNss = nss;
            nsStruct.executionNss = resolvedAggRequest.getNamespaceString();

            return ClusterAggregate::runAggregate(
                opCtx, nsStruct, resolvedAggRequest, resolvedAggCmd, out);
        }

        long long millisElapsed = timer.millis();

        const char* mongosStageName =
            ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

        return ClusterExplain::buildExplainResult(
            opCtx,
            ClusterExplain::downconvert(opCtx, shardResponses),
            mongosStageName,
            millisElapsed,
            out);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        auto query = extractQuery(cmdObj);
        auto collation = extractCollation(cmdObj);

        // Construct collator for deduping.
        std::unique_ptr<CollatorInterface> collator;
        if (!collation.isEmpty()) {
            collator = uassertStatusOK(
                CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        }

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            shardResponses = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                nss.db(),
                nss,
                routingInfo,
                CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent,
                query,
                collation);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
            auto parsedDistinct = ParsedDistinct::parse(
                opCtx, ex->getNamespace(), cmdObj, ExtensionsCallbackNoop(), true);
            uassertStatusOK(parsedDistinct.getStatus());

            auto aggCmdOnView = parsedDistinct.getValue().asAggregationCommand();
            uassertStatusOK(aggCmdOnView.getStatus());

            auto aggRequestOnView = AggregationRequest::parseFromBSON(nss, aggCmdOnView.getValue());
            uassertStatusOK(aggRequestOnView.getStatus());

            auto resolvedAggRequest = ex->asExpandedViewAggregation(aggRequestOnView.getValue());
            auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbName, std::move(resolvedAggCmd)));

            ViewResponseFormatter formatter(aggResult);
            auto formatStatus = formatter.appendAsDistinctResponse(&result);
            uassertStatusOK(formatStatus);

            return true;
        }

        BSONObjComparator bsonCmp(
            BSONObj(),
            BSONObjComparator::FieldNamesMode::kConsider,
            !collation.isEmpty()
                ? collator.get()
                : (routingInfo.cm() ? routingInfo.cm()->getDefaultCollator() : nullptr));
        BSONObjSet all = bsonCmp.makeBSONObjSet();

        for (const auto& response : shardResponses) {
            auto status = response.swResponse.isOK()
                ? getStatusFromCommandResult(response.swResponse.getValue().data)
                : response.swResponse.getStatus();
            uassertStatusOK(status);

            BSONObj res = std::move(response.swResponse.getValue().data);
            BSONObjIterator it(res["values"].embeddedObject());
            while (it.more()) {
                BSONElement nxt = it.next();
                BSONObjBuilder temp(32);
                temp.appendAs(nxt, "");
                all.insert(temp.obj());
            }
        }

        BSONObjBuilder b(32);
        int n = 0;
        for (auto&& obj : all) {
            b.appendAs(obj.firstElement(), b.numStr(n++));
        }

        result.appendArray("values", b.obj());
        return true;
    }

} disinctCmd;

}  // namespace
}  // namespace mongo
