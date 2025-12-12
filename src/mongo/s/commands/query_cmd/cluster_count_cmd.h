/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/query_stats/count_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/query/shard_targeting_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <vector>

namespace mongo {

inline BSONObj prepareCountForPassthrough(const BSONObj& cmdObj, bool requestQueryStats) {
    if (!requestQueryStats) {
        return CommandHelpers::filterCommandRequestForPassthrough(cmdObj);
    }

    BSONObjBuilder bob(cmdObj);
    bob.append("includeQueryStatsMetrics", true);
    return CommandHelpers::filterCommandRequestForPassthrough(bob.done());
}

inline bool convertAndRunAggregateIfViewlessTimeseries(
    OperationContext* const opCtx,
    RoutingContext& routingCtx,
    BSONObjBuilder& bodyBuilder,
    const CountCommandRequest& request,
    const NamespaceString& nss,
    boost::optional<mongo::ExplainOptions::Verbosity> verbosity = boost::none) {
    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
    if (!timeseries::requiresViewlessTimeseriesTranslationInRouter(opCtx, cri)) {
        return false;
    } else {
        // We only need to route the viewless timeseries request to
        // runAggregate() which will perform the pipeline rewrites.
        const auto hasExplain = verbosity.has_value();
        bodyBuilder.resetToEmpty();
        auto aggRequest = query_request_conversion::asAggregateCommandRequest(request, hasExplain);

        uassertStatusOK(ClusterAggregate::runAggregateWithRoutingCtx(
            opCtx,
            routingCtx,
            ClusterAggregate::Namespaces{nss, nss},
            aggRequest,
            {aggRequest},
            {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)},
            boost::none,
            boost::none, /* originalRequest */
            verbosity,
            &bodyBuilder));
        return true;
    }
}

/**
 * Implements the find command on mongos.
 */
template <typename Impl>
class ClusterCountCmdBase final : public ErrmsgCommandDeprecated {
public:
    ClusterCountCmdBase() : ErrmsgCommandDeprecated(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kSnapshotNotSupported{ErrorCodes::InvalidOptions,
                                                  "read concern snapshot not supported"};
        return {{level == repl::ReadConcernLevel::kSnapshotReadConcern, kSnapshotNotSupported},
                Status::OK()};
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

        return Impl::checkAuthForOperation(opCtx, dbName, cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& originalCmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        Impl::checkCanRunHere(opCtx);

        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const NamespaceString originalNss(parseNs(dbName, originalCmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '"
                              << originalNss.toStringForErrorMsg() << "'",
                originalNss.isValid());

        try {
            sharding::router::CollectionRouter router(opCtx, originalNss);
            return router.routeWithRoutingContext(
                getName(), [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
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
                            cmdObj = rewriteCommandForRawDataOperation<CountCommandRequest>(
                                cmdObj, translatedNss.coll());
                            nss = translatedNss;
                        });

                    auto countRequest =
                        CountCommandRequest::parse(cmdObj, IDLParserContext("count"));

                    // Create an RAII object that prints the collection's shard key in the case of a
                    // tassert or crash.
                    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                    ScopedDebugInfo shardKeyDiagnostics(
                        "ShardKeyDiagnostics",
                        diagnostic_printers::ShardKeyDiagnosticPrinter{
                            cri.isSharded() ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                            : BSONObj()});

                    const auto collation = countRequest.getCollation().get_value_or(BSONObj());

                    if (prepareForFLERewrite(opCtx, countRequest.getEncryptionInformation())) {
                        processFLECountS(opCtx, nss, countRequest);
                    }

                    const auto expCtx = makeExpressionContextWithDefaultsForTargeter(
                        opCtx,
                        nss,
                        cri,
                        collation,
                        boost::none /*explainVerbosity*/,
                        boost::none /*letParameters*/,
                        boost::none /*runtimeConstants*/);

                    // Create an RAII object that prints useful information about the
                    // ExpressionContext in the case of a tassert or crash.
                    ScopedDebugInfo expCtxDiagnostics(
                        "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{expCtx});

                    const auto parsedFind = uassertStatusOK(parsed_find_command::parseFromCount(
                        expCtx, countRequest, ExtensionsCallbackNoop(), nss));

                    if (feature_flags::gFeatureFlagQueryStatsCountDistinct.isEnabled(
                            VersionContext::getDecoration(opCtx),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                        query_stats::registerRequest(opCtx, nss, [&]() {
                            return std::make_unique<query_stats::CountKey>(
                                expCtx,
                                *parsedFind,
                                countRequest.getLimit().has_value(),
                                countRequest.getSkip().has_value(),
                                countRequest.getReadConcern(),
                                countRequest.getMaxTimeMS().has_value());
                        });
                    }

                    // Note: This must happen after query stats because query stats retain the
                    // originating command type for timeseries.
                    {
                        // This scope is used to end the use of the builder whether or not we
                        // convert to a view-less timeseries aggregate request.
                        auto aggResult = BSONObjBuilder{};

                        if (convertAndRunAggregateIfViewlessTimeseries(
                                opCtx, routingCtx, aggResult, countRequest, nss)) {
                            ViewResponseFormatter{aggResult.obj()}.appendAsCountResponse(
                                &result, boost::none /*tenantId*/);
                            return true;
                        }
                    }

                    // We only need to factor in the skip value when sending to the shards if we
                    // have a value for limit, otherwise, we apply it only once we have collected
                    // all counts.
                    if (countRequest.getLimit() && countRequest.getSkip()) {
                        const auto limit = countRequest.getLimit().value();
                        const auto skip = countRequest.getSkip().value();
                        if (limit != 0) {
                            std::int64_t sum = 0;
                            uassert(ErrorCodes::Overflow,
                                    str::stream() << "Overflow on the count command: The sum of "
                                                     "the limit and skip "
                                                     "fields must fit into a long integer. Limit: "
                                                  << limit << "   Skip: " << skip,
                                    !overflow::add(limit, skip, &sum));
                            countRequest.setLimit(sum);
                        }
                    }
                    countRequest.setSkip(boost::none);

                    // The includeQueryStatsMetrics field is not supported on mongos for the count
                    // command, so we do not need to check the value on the original request when
                    // updating requestQueryStats here.
                    bool requestQueryStats =
                        query_stats::shouldRequestRemoteMetrics(CurOp::get(opCtx)->debug());

                    std::vector<AsyncRequestsSender::Response> shardResponses;
                    try {
                        shardResponses = scatterGatherVersionedTargetByRoutingTable(
                            expCtx,
                            routingCtx,
                            nss,
                            applyReadWriteConcern(opCtx,
                                                  this,
                                                  prepareCountForPassthrough(countRequest.toBSON(),
                                                                             requestQueryStats)),
                            ReadPreferenceSetting::get(opCtx),
                            Shard::RetryPolicy::kIdempotent,
                            countRequest.getQuery(),
                            collation,
                            true /*eligibleForSampling*/);

                    } catch (
                        const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&
                            ex) {
                        // Rewrite the count command as an aggregation.
                        auto countRequest =
                            CountCommandRequest::parse(originalCmdObj, IDLParserContext("count"));
                        auto aggRequestOnView =
                            query_request_conversion::asAggregateCommandRequest(countRequest);

                        const ResolvedView& resolvedView = *ex.extraInfo<ResolvedView>();
                        auto resolvedAggRequest =
                            PipelineResolver::buildRequestWithResolvedPipeline(resolvedView,
                                                                               aggRequestOnView);

                        BSONObj aggResult = CommandHelpers::runCommandDirectly(
                            opCtx,
                            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                                        dbName,
                                                        resolvedAggRequest.toBSON()));

                        result.resetToEmpty();
                        ViewResponseFormatter{aggResult}.appendAsCountResponse(
                            &result, boost::none /*tenantId*/);
                        return true;
                    }

                    long long total = 0;
                    bool allShardMetricsReturned = true;
                    BSONObjBuilder shardSubTotal(result.subobjStart("shards"));

                    for (const auto& response : shardResponses) {
                        auto status = response.swResponse.getStatus();
                        if (status.isOK()) {
                            status =
                                getStatusFromCommandResult(response.swResponse.getValue().data);
                            if (status.isOK()) {
                                const BSONObj& data = response.swResponse.getValue().data;
                                const long long shardCount = data["n"].numberLong();
                                shardSubTotal.appendNumber(response.shardId.toString(), shardCount);
                                total += shardCount;

                                // We aggregate the metrics from all the shards. If any shard does
                                // not include metrics, we avoid collecting the remote metrics for
                                // the entire query and do not write an entry to the query stats
                                // store. Note that we do not expect shards to fail to collect
                                // metrics for the count command; this is just thorough error
                                // handling.
                                BSONElement shardMetrics = data["metrics"];
                                if (allShardMetricsReturned &= shardMetrics.isABSONObj()) {
                                    const auto metrics = CursorMetrics::parse(
                                        shardMetrics.Obj(), IDLParserContext("CursorMetrics"));
                                    CurOp::get(opCtx)
                                        ->debug()
                                        .getAdditiveMetrics()
                                        .aggregateCursorMetrics(metrics);
                                }
                                continue;
                            }
                        }

                        shardSubTotal.doneFast();
                        // Add error context so that you can see on which shard failed as well as
                        // details about that error.
                        uassertStatusOK(
                            status.withContext(str::stream() << "failed on: " << response.shardId));
                    }

                    shardSubTotal.doneFast();
                    total = applySkipLimit(total, originalCmdObj);
                    result.appendNumber("n", total);

                    // The # of documents returned is always 1 for the count command.
                    static constexpr long long kNReturned = 1;

                    auto* curOp = CurOp::get(opCtx);
                    curOp->setEndOfOpMetrics(kNReturned);

                    if (allShardMetricsReturned) {
                        collectQueryStatsMongos(opCtx,
                                                std::move(curOp->debug().getQueryStatsInfo().key));
                    }

                    return true;
                });
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            result.resetToEmpty();

            // If there's no collection with this name, the count aggregation behavior below
            // will produce a total count of 0.
            result.appendNumber("n", 0);

            // The # of documents returned is always 1 for the count command.
            auto* curOp = CurOp::get(opCtx);
            curOp->setEndOfOpMetrics(1);

            collectQueryStatsMongos(opCtx, std::move(curOp->debug().getQueryStatsInfo().key));
            return true;
        }
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        Impl::checkCanExplainHere(opCtx);

        const BSONObj& originalCmdObj = request.body;

        auto curOp = CurOp::get(opCtx);
        curOp->debug().getQueryStatsInfo().disableForSubqueryExecution = true;

        const auto originalNss = parseNs(request.parseDbName(), originalCmdObj);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '"
                              << originalNss.toStringForErrorMsg() << "'",
                originalNss.isValid());

        sharding::router::CollectionRouter router(opCtx, originalNss);
        return router.routeWithRoutingContext(
            "explain count"_sd, [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                // Clear the bodyBuilder since this lambda function may be retried if the router
                // cache is stale.
                result->getBodyBuilder().resetToEmpty();

                // Transform the nss, routingCtx and cmdObj if the 'rawData' field is enabled and
                // the collection is timeseries.
                BSONObj cmdObj = originalCmdObj;
                auto nss = originalNss;
                const auto targeter = CollectionRoutingInfoTargeter(opCtx, originalNss);
                auto& routingCtx = performTimeseriesTranslationAccordingToRoutingInfo(
                    opCtx,
                    originalNss,
                    targeter,
                    originalRoutingCtx,
                    [&](const NamespaceString& translatedNss) {
                        cmdObj = rewriteCommandForRawDataOperation<CountCommandRequest>(
                            cmdObj, translatedNss.coll());
                        nss = translatedNss;
                    });

                CountCommandRequest countRequest(NamespaceStringOrUUID(NamespaceString{}));
                try {
                    countRequest = CountCommandRequest::parse(cmdObj, IDLParserContext("count"));
                } catch (...) {
                    routingCtx.skipValidation();
                    return exceptionToStatus();
                }

                // Create an RAII object that prints the collection's shard key in the case of a
                // tassert or crash.
                const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                ScopedDebugInfo shardKeyDiagnostics(
                    "ShardKeyDiagnostics",
                    diagnostic_printers::ShardKeyDiagnosticPrinter{
                        cri.isSharded() ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                        : BSONObj()});

                {
                    // This scope is used to end the use of the builder whether or not we convert to
                    // a view-less timeseries aggregate request.
                    auto bodyBuilder = result->getBodyBuilder();
                    if (convertAndRunAggregateIfViewlessTimeseries(
                            opCtx, routingCtx, bodyBuilder, countRequest, nss, verbosity)) {
                        // We've delegated execution to agg.
                        return Status::OK();
                    }
                }

                // If the command has encryptionInformation, rewrite the query as necessary.
                if (prepareForFLERewrite(opCtx, countRequest.getEncryptionInformation())) {
                    processFLECountS(opCtx, nss, countRequest);
                }

                BSONObj targetingQuery = countRequest.getQuery();
                BSONObj targetingCollation = countRequest.getCollation().value_or(BSONObj());

                auto expCtx = makeBlankExpressionContext(opCtx, nss);
                auto numShards =
                    getTargetedShardsForQuery(expCtx, cri, targetingQuery, targetingCollation)
                        .size();
                auto userLimit = countRequest.getLimit();
                auto userSkip = countRequest.getSkip();
                if (numShards > 1) {
                    // If there is a limit, we forward the sum of the limit and skip.
                    auto swNewLimit =
                        addLimitAndSkipForShards(countRequest.getLimit(), countRequest.getSkip());
                    if (!swNewLimit.isOK()) {
                        return swNewLimit.getStatus();
                    }
                    countRequest.setLimit(swNewLimit.getValue());
                    countRequest.setSkip(boost::none);
                }

                const auto explainCmd =
                    ClusterExplain::wrapAsExplain(countRequest.toBSON(), verbosity);

                // We will time how long it takes to run the commands on the shards
                Timer timer;

                std::vector<AsyncRequestsSender::Response> shardResponses;
                try {
                    shardResponses = scatterGatherVersionedTargetByRoutingTable(
                        opCtx,
                        routingCtx,
                        nss,
                        explainCmd,
                        ReadPreferenceSetting::get(opCtx),
                        Shard::RetryPolicy::kIdempotent,
                        targetingQuery,
                        targetingCollation,
                        boost::none /*letParameters*/,
                        boost::none /*runtimeConstants*/);
                } catch (
                    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                    CountCommandRequest countRequest(NamespaceStringOrUUID(NamespaceString{}));
                    try {
                        countRequest =
                            CountCommandRequest::parse(cmdObj, IDLParserContext("count"));
                    } catch (...) {
                        return exceptionToStatus();
                    }
                    auto aggRequestOnView = query_request_conversion::asAggregateCommandRequest(
                        countRequest, true /* hasExplain */);
                    auto bodyBuilder = result->getBodyBuilder();
                    // An empty PrivilegeVector is acceptable because these privileges are only
                    // checked on getMore and explain will not open a cursor.
                    return ClusterAggregate::retryOnViewError(opCtx,
                                                              aggRequestOnView,
                                                              *ex.extraInfo<ResolvedView>(),
                                                              nss,
                                                              PrivilegeVector(),
                                                              verbosity,
                                                              &bodyBuilder);
                }

                long long millisElapsed = timer.millis();

                const char* mongosStageName =
                    ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

                auto bodyBuilder = result->getBodyBuilder();
                return ClusterExplain::buildExplainResult(expCtx,
                                                          shardResponses,
                                                          mongosStageName,
                                                          millisElapsed,
                                                          originalCmdObj,
                                                          &bodyBuilder,
                                                          userLimit,
                                                          userSkip);
            });
    }

private:
    static long long applySkipLimit(long long num, const BSONObj& cmd) {
        BSONElement s = cmd["skip"];
        BSONElement l = cmd["limit"];

        if (s.isNumber()) {
            num = num - s.safeNumberLong();
            if (num < 0) {
                num = 0;
            }
        }

        if (l.isNumber()) {
            auto limit = l.safeNumberLong();
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
};

}  // namespace mongo
