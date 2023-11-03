/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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


#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class DistinctCmd : public BasicCommand {
public:
    DistinctCmd() : BasicCommand("distinct") {}

    std::string help() const override {
        return "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {
        return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool allowedInTransactions() const final {
        return true;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& opMsgRequest,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        const BSONObj& cmdObj = opMsgRequest.body;
        const NamespaceString nss(parseNs(opMsgRequest.getDbName(), cmdObj));
        auto canonicalDistinct = CanonicalDistinct::parseFromBSON(
            opCtx, nss, cmdObj, ExtensionsCallbackNoop(), nullptr /* defaultCollator */, verbosity);
        auto canonicalQuery = canonicalDistinct.getQuery();
        auto targetingQuery = canonicalQuery->getQueryObj();
        auto targetingCollation = canonicalQuery->getFindCommandRequest().getCollation();
        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            shardResponses =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           nss.dbName(),
                                                           nss,
                                                           cri,
                                                           explainCmd,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           targetingQuery,
                                                           targetingCollation,
                                                           boost::none /*letParameters*/,
                                                           boost::none /*runtimeConstants*/);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {

            auto aggCmdOnView = canonicalDistinct.asAggregationCommand();
            if (!aggCmdOnView.isOK()) {
                return aggCmdOnView.getStatus();
            }

            auto viewAggCmd =
                OpMsgRequest::fromDBAndBody(nss.dbName(), aggCmdOnView.getValue()).body;
            auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                opCtx,
                nss,
                viewAggCmd,
                verbosity,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));


            auto bodyBuilder = result->getBodyBuilder();
            // An empty PrivilegeVector is acceptable because these privileges are only checked on
            // getMore and explain will not open a cursor.
            return ClusterAggregate::retryOnViewError(opCtx,
                                                      aggRequestOnView,
                                                      *ex.extraInfo<ResolvedView>(),
                                                      nss,
                                                      PrivilegeVector(),
                                                      &bodyBuilder);
        }

        long long millisElapsed = timer.millis();

        const char* mongosStageName =
            ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

        auto bodyBuilder = result->getBodyBuilder();
        return ClusterExplain::buildExplainResult(
            opCtx, shardResponses, mongosStageName, millisElapsed, cmdObj, &bodyBuilder);
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const NamespaceString nss(parseNs(dbName, cmdObj));
        auto canonicalDistinct = CanonicalDistinct::parseFromBSON(
            opCtx, nss, cmdObj, ExtensionsCallbackNoop(), nullptr /* defaultCollator */);
        auto canonicalQuery = canonicalDistinct.getQuery();
        auto query = canonicalQuery->getQueryObj();
        auto collation = canonicalQuery->getFindCommandRequest().getCollation();

        auto swCri = getCollectionRoutingInfoForTxnCmd(opCtx, nss);
        if (swCri == ErrorCodes::NamespaceNotFound) {
            // If the database doesn't exist, we successfully return an empty result set without
            // creating a cursor.
            result.appendArray("values", BSONObj());
            return true;
        }

        const auto cri = uassertStatusOK(std::move(swCri));
        const auto& cm = cri.cm;
        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            shardResponses = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                nss.dbName(),
                nss,
                cri,
                applyReadWriteConcern(
                    opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent,
                query,
                collation,
                boost::none /*letParameters*/,
                boost::none /*runtimeConstants*/,
                true /* eligibleForSampling */);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
            auto aggCmdOnView = canonicalDistinct.asAggregationCommand();
            uassertStatusOK(aggCmdOnView.getStatus());

            auto viewAggCmd =
                OpMsgRequest::fromDBAndBody(nss.dbName(), aggCmdOnView.getValue()).body;
            auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                opCtx,
                nss,
                viewAggCmd,
                boost::none,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));

            auto resolvedAggRequest = ex->asExpandedViewAggregation(aggRequestOnView);
            auto resolvedAggCmd =
                aggregation_request_helper::serializeToCommandObj(resolvedAggRequest);

            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                txnRouter.onViewResolutionError(opCtx, nss);
            }

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx, OpMsgRequestBuilder::create(dbName, std::move(resolvedAggCmd)));

            ViewResponseFormatter formatter(aggResult);
            auto formatStatus = formatter.appendAsDistinctResponse(&result, boost::none);
            uassertStatusOK(formatStatus);

            return true;
        }

        BSONObjComparator bsonCmp(BSONObj(),
                                  BSONObjComparator::FieldNamesMode::kConsider,
                                  !collation.isEmpty()
                                      ? canonicalQuery->getCollator()
                                      : (cm.isSharded() ? cm.getDefaultCollator() : nullptr));
        BSONObjSet all = bsonCmp.makeBSONObjSet();

        for (const auto& response : shardResponses) {
            auto status = response.swResponse.isOK()
                ? getStatusFromCommandResult(response.swResponse.getValue().data)
                : response.swResponse.getStatus();
            uassertStatusOK(status);

            BSONObj res = response.swResponse.getValue().data;
            auto values = res["values"];
            uassert(5986900,
                    str::stream() << "No 'values' field in distinct command response: "
                                  << res.toString() << ". Original command: " << cmdObj.toString(),
                    !values.eoo());
            uassert(5986901,
                    str::stream() << "Expected 'values' field to be of type Array, but found "
                                  << typeName(values.type()),
                    values.type() == BSONType::Array);
            BSONObjIterator it(values.embeddedObject());
            while (it.more()) {
                BSONElement nxt = it.next();
                BSONObjBuilder temp(32);
                temp.appendAs(nxt, "");
                all.insert(temp.obj());
            }
        }

        BSONObjBuilder b(32);
        DecimalCounter<unsigned> n;
        for (auto&& obj : all) {
            b.appendAs(obj.firstElement(), StringData{n});
            ++n;
        }

        result.appendArray("values", b.obj());
        // If mongos selected atClusterTime or received it from client, transmit it back.
        if (!opCtx->inMultiDocumentTransaction() &&
            repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            result.append("atClusterTime"_sd,
                          repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()->asTimestamp());
        }
        return true;
    }
};
MONGO_REGISTER_COMMAND(DistinctCmd).forRouter();

}  // namespace
}  // namespace mongo
