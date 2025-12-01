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
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/distinct_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

std::unique_ptr<CanonicalQuery> parseDistinctCmd(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ExtensionsCallback& extensionsCallback,
    const CollatorInterface* defaultCollator,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    const auto serializationContext = vts.has_value()
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();

    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        cmdObj,
        IDLParserContext("distinctCommandRequest", vts, nss.tenantId(), serializationContext)));

    // Forbid users from passing 'querySettings' explicitly.
    uassert(7923001,
            "BSON field 'querySettings' is an unknown field",
            !distinctCommand->getQuerySettings().has_value());

    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, *distinctCommand, defaultCollator)
                      .ns(nss)
                      .explain(verbosity)
                      .build();
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx,
                                       std::move(distinctCommand),
                                       extensionsCallback,
                                       MatchExpressionParser::kAllowAllSpecialFeatures);

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::DistinctCmdShape>(*parsedDistinct, expCtx);
    }};
    auto queryShapeHash = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() { return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nss); });

    // Perform the query settings lookup and attach it to 'expCtx'.
    auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx);
    auto querySettings =
        querySettingsService.lookupQuerySettingsWithRejectionCheck(expCtx, queryShapeHash, nss);
    expCtx->setQuerySettingsIfNotPresent(std::move(querySettings));

    // We do not collect queryStats on explain for distinct.
    if (feature_flags::gFeatureFlagQueryStatsCountDistinct.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        !verbosity.has_value()) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
            return std::make_unique<query_stats::DistinctKey>(
                expCtx,
                *parsedDistinct->distinctCommandRequest,
                std::move(deferredShape->getValue()));
        });
    }

    return parsed_distinct_command::parseCanonicalQuery(std::move(expCtx),
                                                        std::move(parsedDistinct));
}

BSONObj prepareDistinctForPassthrough(const BSONObj& cmd,
                                      const query_settings::QuerySettings& qs,
                                      const bool requestQueryStats) {
    const auto qsBson = qs.toBSON();
    if (requestQueryStats || !qsBson.isEmpty()) {
        BSONObjBuilder bob(cmd);
        // Append distinct command with the query settings and includeQueryStatsMetrics if needed.
        if (requestQueryStats) {
            bob.append("includeQueryStatsMetrics", true);
        }
        if (!qsBson.isEmpty()) {
            bob.append("querySettings", qsBson);
        }
        return CommandHelpers::filterCommandRequestForPassthrough(bob.done());
    }

    return CommandHelpers::filterCommandRequestForPassthrough(cmd);
}

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

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {
        return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    }

    bool supportsRawData() const override {
        return true;
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
        const BSONObj& originalCmdObj = opMsgRequest.body;
        const NamespaceString originalNss(parseNs(opMsgRequest.parseDbName(), originalCmdObj));

        sharding::router::CollectionRouter router{opCtx->getServiceContext(), originalNss};
        return router.routeWithRoutingContext(
            opCtx,
            "explain distinct"_sd,
            [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                // Clear the bodyBuilder since this lambda function may be retried if the router
                // cache is stale.
                result->getBodyBuilder().resetToEmpty();

                // Transform the nss, routingCtx and cmdObj if the 'rawData' field is enabled and
                // the collection is timeseries.
                BSONObj cmdObj = originalCmdObj;
                auto nss = originalNss;
                const auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
                auto& routingCtx = performTimeseriesTranslationAccordingToRoutingInfo(
                    opCtx,
                    originalNss,
                    targeter,
                    originalRoutingCtx,
                    [&](const NamespaceString& translatedNss) {
                        cmdObj = rewriteCommandForRawDataOperation<DistinctCommandRequest>(
                            cmdObj, translatedNss.coll());
                        nss = translatedNss;
                    });

                auto canonicalQuery = parseDistinctCmd(opCtx,
                                                       nss,
                                                       cmdObj,
                                                       ExtensionsCallbackNoop(),
                                                       nullptr /* defaultCollator */,
                                                       verbosity);

                // Create an RAII object that prints useful information about the ExpressionContext
                // in the case of a tassert or crash.
                ScopedDebugInfo expCtxDiagnostics(
                    "ExpCtxDiagnostics",
                    diagnostic_printers::ExpressionContextPrinter{canonicalQuery->getExpCtx()});

                auto targetingQuery = canonicalQuery->getQueryObj();
                auto targetingCollation = canonicalQuery->getFindCommandRequest().getCollation();

                // We will time how long it takes to run the commands on the shards.
                Timer timer;

                // Clear the bodyBuilder since this lambda function may be retried if the router
                // cache is stale.
                auto bodyBuilder = result->getBodyBuilder();
                bodyBuilder.resetToEmpty();

                std::vector<AsyncRequestsSender::Response> shardResponses;

                // Create an RAII object that prints the collection's shard key in the case of a
                // tassert or crash.
                const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                ScopedDebugInfo shardKeyDiagnostics(
                    "ShardKeyDiagnostics",
                    diagnostic_printers::ShardKeyDiagnosticPrinter{
                        cri.isSharded() ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                        : BSONObj()});

                if (timeseries::requiresViewlessTimeseriesTranslationInRouter(opCtx, cri)) {
                    runDistinctAsAgg(opCtx,
                                     routingCtx,
                                     std::move(canonicalQuery),
                                     boost::none /* resolvedView */,
                                     verbosity,
                                     bodyBuilder);
                    return Status::OK();
                } else {
                    try {
                        shardResponses = scatterGatherVersionedTargetByRoutingTable(
                            opCtx,
                            routingCtx,
                            nss,
                            ClusterExplain::wrapAsExplain(
                                cmdObj,
                                verbosity,
                                canonicalQuery->getExpCtx()->getQuerySettings().toBSON()),
                            ReadPreferenceSetting::get(opCtx),
                            Shard::RetryPolicy::kIdempotent,
                            targetingQuery,
                            targetingCollation,
                            boost::none /*letParameters*/,
                            boost::none /*runtimeConstants*/);
                    } catch (
                        const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&
                            ex) {
                        const auto& resolvedView = *ex.extraInfo<ResolvedView>();
                        runDistinctAsAgg(opCtx,
                                         routingCtx,
                                         std::move(canonicalQuery),
                                         resolvedView,
                                         verbosity,
                                         bodyBuilder);
                        return Status::OK();
                    }
                }

                long long millisElapsed = timer.millis();

                const char* mongosStageName =
                    ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

                return ClusterExplain::buildExplainResult(makeBlankExpressionContext(opCtx, nss),
                                                          shardResponses,
                                                          mongosStageName,
                                                          millisElapsed,
                                                          originalCmdObj,
                                                          &bodyBuilder);
            });
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& originalCmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        NamespaceString originalNss(parseNs(dbName, originalCmdObj));
        try {
            sharding::router::CollectionRouter router{opCtx->getServiceContext(), originalNss};
            return router.routeWithRoutingContext(
                opCtx, getName(), [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                    // Clear the bodyBuilder since this lambda function may be retried if the router
                    // cache is stale.
                    result.resetToEmpty();

                    // Transform the nss, routingCtx and cmdObj if the 'rawData' field is enabled
                    // and the collection is timeseries.
                    BSONObj cmdObj = originalCmdObj;
                    auto nss = originalNss;
                    const auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
                    auto& routingCtx = performTimeseriesTranslationAccordingToRoutingInfo(
                        opCtx,
                        originalNss,
                        targeter,
                        originalRoutingCtx,
                        [&](const NamespaceString& translatedNss) {
                            cmdObj = rewriteCommandForRawDataOperation<DistinctCommandRequest>(
                                cmdObj, translatedNss.coll());
                            nss = translatedNss;
                        });

                    auto canonicalQuery = parseDistinctCmd(opCtx,
                                                           nss,
                                                           cmdObj,
                                                           ExtensionsCallbackNoop(),
                                                           nullptr /* defaultCollator */,
                                                           boost::none /* verbosity */);
                    auto query = canonicalQuery->getQueryObj();
                    auto collation = canonicalQuery->getFindCommandRequest().getCollation();

                    // Create an RAII object that prints useful information about the
                    // ExpressionContext in the case of a tassert or crash.
                    ScopedDebugInfo expCtxDiagnostics(
                        "ExpCtxDiagnostics",
                        diagnostic_printers::ExpressionContextPrinter{canonicalQuery->getExpCtx()});

                    // Users cannot set 'includeQueryStatsMetrics' for distinct commands on mongos.
                    // We will decide if remote query stats metrics should be collected.
                    bool requestQueryStats =
                        query_stats::shouldRequestRemoteMetrics(CurOp::get(opCtx)->debug());

                    BSONObj distinctReadyForPassthrough = prepareDistinctForPassthrough(
                        cmdObj, canonicalQuery->getExpCtx()->getQuerySettings(), requestQueryStats);


                    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                    const auto& cm = cri.getChunkManager();

                    // Create an RAII object that prints the collection's shard key in the case of a
                    // tassert or crash.
                    ScopedDebugInfo shardKeyDiagnostics(
                        "ShardKeyDiagnostics",
                        diagnostic_printers::ShardKeyDiagnosticPrinter{
                            cri.isSharded() ? cm.getShardKeyPattern().toBSON() : BSONObj()});

                    std::vector<AsyncRequestsSender::Response> shardResponses;
                    if (timeseries::requiresViewlessTimeseriesTranslationInRouter(opCtx, cri)) {
                        runDistinctAsAgg(opCtx,
                                         routingCtx,
                                         std::move(canonicalQuery),
                                         boost::none /* resolvedView */,
                                         boost::none /* verbosity */,
                                         result);
                        return true;
                    }

                    try {
                        shardResponses = scatterGatherVersionedTargetByRoutingTable(
                            opCtx,
                            routingCtx,
                            nss,
                            applyReadWriteConcern(opCtx, this, distinctReadyForPassthrough),
                            ReadPreferenceSetting::get(opCtx),
                            Shard::RetryPolicy::kIdempotent,
                            query,
                            collation,
                            boost::none /*letParameters*/,
                            boost::none /*runtimeConstants*/,
                            true /* eligibleForSampling */);
                    } catch (
                        const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&
                            ex) {
                        const auto& resolvedView = *ex.extraInfo<ResolvedView>();
                        runDistinctAsAgg(opCtx,
                                         routingCtx,
                                         std::move(canonicalQuery),
                                         resolvedView,
                                         boost::none /* verbosity */,
                                         result);
                        return true;
                    }

                    // The construction of 'bsonCmp' below only accounts for a collection default
                    // collator when the targeted collection is sharded. In the event that the
                    // collection is unsharded (either untracked or unsplittable) and has a
                    // collection default collator, we can use binary comparison on the router as
                    // long as we obey the collection's default collator on the targeted shard.
                    // TODO SERVER-101576: Setting up the collation and aggregating shard responses
                    // can be avoided entirely when targeting a single shard.
                    BSONObjComparator bsonCmp(
                        BSONObj(),
                        BSONObjComparator::FieldNamesMode::kConsider,
                        !collation.isEmpty()
                            ? canonicalQuery->getCollator()
                            : (cri.isSharded() ? cm.getDefaultCollator() : nullptr));
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
                                              << res.toString()
                                              << ". Original command: " << cmdObj.toString(),
                                !values.eoo());
                        uassert(5986901,
                                str::stream()
                                    << "Expected 'values' field to be of type Array, but found "
                                    << typeName(values.type()),
                                values.type() == BSONType::array);
                        BSONObjIterator it(values.embeddedObject());
                        while (it.more()) {
                            BSONElement nxt = it.next();
                            BSONObjBuilder temp(32);
                            temp.appendAs(nxt, "");
                            all.insert(temp.obj());
                        }

                        if (requestQueryStats) {
                            BSONElement shardMetrics = res["metrics"];
                            if (shardMetrics.isABSONObj()) {
                                auto metrics = CursorMetrics::parse(
                                    shardMetrics.Obj(), IDLParserContext("CursorMetrics"));
                                CurOp::get(opCtx)->debug().additiveMetrics.aggregateCursorMetrics(
                                    metrics);
                            }
                        }
                    }

                    BSONObjBuilder b(32);
                    DecimalCounter<unsigned> n;
                    for (auto&& obj : all) {
                        b.appendAs(obj.firstElement(), StringData{n});
                        ++n;
                    }

                    result.appendArray("values", b.obj());
                    // If mongos selected atClusterTime or received it from client, transmit it
                    // back.
                    if (!opCtx->inMultiDocumentTransaction() &&
                        repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
                        result.append("atClusterTime"_sd,
                                      repl::ReadConcernArgs::get(opCtx)
                                          .getArgsAtClusterTime()
                                          ->asTimestamp());
                    }

                    CurOp::get(opCtx)->setEndOfOpMetrics(n);
                    collectQueryStatsMongos(
                        opCtx, std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));

                    return true;
                });
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            result.resetToEmpty();

            // Register the query into the query stats.
            auto canonicalQuery = parseDistinctCmd(opCtx,
                                                   originalNss,
                                                   originalCmdObj,
                                                   ExtensionsCallbackNoop(),
                                                   nullptr /* defaultCollator */,
                                                   boost::none /* verbosity */);

            // If the database doesn't exist, we successfully return an empty result set.
            result.appendArray("values", BSONObj());
            CurOp::get(opCtx)->setEndOfOpMetrics(0);
            collectQueryStatsMongos(opCtx,
                                    std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
            return true;
        }
    }

    void runDistinctAsAgg(OperationContext* opCtx,
                          RoutingContext& routingCtx,
                          std::unique_ptr<CanonicalQuery> canonicalQuery,
                          boost::optional<const ResolvedView&> resolvedView,
                          boost::optional<ExplainOptions::Verbosity> verbosity,
                          BSONObjBuilder& bob) const {
        const auto& nss = canonicalQuery->nss();
        const auto& dbName = nss.dbName();
        auto distinctAggRequest = parsed_distinct_command::asAggregation(
            *canonicalQuery,
            verbosity,
            canonicalQuery->getFindCommandRequest().getSerializationContext());

        // Propagate the query settings with the request to the shards if present.
        const auto& querySettings = canonicalQuery->getExpCtx()->getQuerySettings();
        if (!query_settings::isDefault(querySettings)) {
            distinctAggRequest.setQuerySettings(querySettings);
        }

        auto curOp = CurOp::get(opCtx);

        // We must store the key in distinct to prevent collecting query stats when the aggregation
        // runs.
        auto ownedQueryStatsKey = std::move(curOp->debug().queryStatsInfo.key);
        curOp->debug().queryStatsInfo.disableForSubqueryExecution = true;

        // Skip privilege checking if we are in an explain.
        if (verbosity) {
            if (resolvedView.has_value()) {
                // If running explain distinct on view, then aggregate is executed without privilege
                // checks and without response formatting.
                uassertStatusOK(ClusterAggregate::retryOnViewError(opCtx,
                                                                   distinctAggRequest,
                                                                   *resolvedView,
                                                                   nss,
                                                                   PrivilegeVector(),
                                                                   verbosity,
                                                                   &bob));
                // Skip routingCtx validation here as a new context and routing table will be
                // acquired for the resolved view.
                routingCtx.skipValidation();
            } else {
                // Viewless timeseries, similar idea.
                uassertStatusOK(ClusterAggregate::runAggregateWithRoutingCtx(
                    opCtx,
                    routingCtx,
                    ClusterAggregate::Namespaces{nss, nss},
                    distinctAggRequest,
                    {distinctAggRequest},
                    PrivilegeVector{},
                    boost::none,
                    boost::none, /* originalRequest */
                    verbosity,
                    &bob));
            }
            return;
        }

        const auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(opCtx,
                                            AuthorizationSession::get(opCtx->getClient()),
                                            distinctAggRequest.getNamespace(),
                                            distinctAggRequest,
                                            true /* isMongos */));
        if (resolvedView.has_value()) {
            // Query against a view.
            uassertStatusOK(ClusterAggregate::retryOnViewError(
                opCtx, distinctAggRequest, *resolvedView, nss, privileges, verbosity, &bob));
            // Skip routingCtx validation here as new contexts and routing tables will be acquired
            // for the resolved view.
            routingCtx.skipValidation();
        } else {
            // Query against viewless timeseries.
            uassertStatusOK(
                ClusterAggregate::runAggregateWithRoutingCtx(opCtx,
                                                             routingCtx,
                                                             ClusterAggregate::Namespaces{nss, nss},
                                                             distinctAggRequest,
                                                             {distinctAggRequest},
                                                             privileges,
                                                             boost::none,
                                                             boost::none, /* originalRequest */
                                                             verbosity,
                                                             &bob));
        }

        // Copy the result from the aggregate command.
        CommandHelpers::extractOrAppendOk(bob);
        ViewResponseFormatter responseFormatter(bob.asTempObj().copy());

        // Reset the builder state, as the response will be written to the same builder.
        bob.resetToEmpty();
        uassertStatusOK(responseFormatter.appendAsDistinctResponse(&bob, dbName.tenantId()));

        curOp->setEndOfOpMetrics(bob.asTempObj().getObjectField("values").nFields());
        collectQueryStatsMongos(opCtx, std::move(ownedQueryStatsKey));
    }
};
MONGO_REGISTER_COMMAND(DistinctCmd).forRouter();

}  // namespace
}  // namespace mongo
