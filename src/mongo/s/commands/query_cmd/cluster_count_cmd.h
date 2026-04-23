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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/count_cmd_helper.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/query_shape/count_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_stats/count_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/pipeline_resolver.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/query/shard_targeting_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <vector>

namespace mongo {

inline BSONObj prepareCountForPassthrough(const OperationContext* opCtx,
                                          const BSONObj& cmdObj,
                                          bool requestQueryStats) {
    BSONObjBuilder bob(cmdObj);

    // Pass the queryShapeHash to the shards. We must validate that all participating shards can
    // understand 'originalQueryShapeHash' and therefore check the feature flag. We use the last
    // LTS when the FCV is uninitialized, since count commands can run during initial sync. This is
    // because the feature is exclusively for observability enhancements and should only be applied
    // when we are confident that the shard can correctly read this field, ensuring the query will
    // not error.
    if (feature_flags::gFeatureFlagOriginalQueryShapeHash.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (auto&& queryShapeHash = CurOp::get(opCtx)->debug().getQueryShapeHash()) {
            bob.append(CountCommandRequest::kOriginalQueryShapeHashFieldName,
                       queryShapeHash->toHexString());
        }
    }
    if (requestQueryStats) {
        bob.append(CountCommandRequest::kIncludeQueryStatsMetricsFieldName, true);
    }

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

inline void createShapeAndRegisterQueryStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             const CountCommandRequest& countRequest,
                                             const NamespaceString& nss) {
    const std::unique_ptr<ParsedFindCommand> parsedFind = uassertStatusOK(
        parsed_find_command::parseFromCount(expCtx, countRequest, ExtensionsCallbackNoop(), nss));

    // Compute QueryShapeHash and record it in CurOp.
    OperationContext* opCtx = expCtx->getOperationContext();
    const query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::CountCmdShape>(
            *parsedFind, countRequest.getLimit().has_value(), countRequest.getSkip().has_value());
    }};
    boost::optional<query_shape::QueryShapeHash> queryShapeHash =
        CurOp::get(opCtx)->debug().ensureQueryShapeHash(opCtx, [&]() {
            return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nss);
        });

    if (feature_flags::gFeatureFlagQueryStatsCountDistinct.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        query_stats::registerRequest(opCtx, nss, [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
            return std::make_unique<query_stats::CountKey>(
                expCtx, countRequest, std::move(deferredShape->getValue()));
        });
    }
}

/**
 * Implements the count command on mongos.
 *
 * The 'Impl' template parameter is a small struct that provides the properties that differ between
 * the 'ClusterCountCmdD' and 'ClusterCountCmdS' variants, such as:
 *  - The command name.
 *  - The stable API version of the command via 'getApiVersions()'.
 *  - Whether the command or its explain can run via 'checkCanRunHere()' and
 *  'checkCanExplainHere()'.
 *
 * This class uses CRTP with 'TypedCommand' where:
 *  - The type ClusterCountCmdBase<Impl> is passed as the concrete type for the template parameter.
 *  - 'Impl' provides the command-specific behavior.
 *
 * This template class provides the shared functionality between the command variants such as the
 * main execution path - 'run()' and 'explain()'.
 *
 * See 'cluster_count_cmd_s.cpp' and 'cluster_count_cmd_s.cpp' for more
 * details.
 */
template <typename Impl>
class ClusterCountCmdBase final : public TypedCommand<ClusterCountCmdBase<Impl>> {
public:
    using TC = TypedCommand<ClusterCountCmdBase<Impl>>;
    using Request = typename Impl::Request;
    using Reply = typename Impl::Reply;
    ClusterCountCmdBase() : TC(Impl::kCommandName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return TC::AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    typename TC::ReadWriteType getReadWriteType() const override {
        return TC::ReadWriteType::kRead;
    }

    class Invocation final : public TC::InvocationBase {
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;
        using TC::InvocationBase::unparsedRequest;

    public:
        Reply typedRun(OperationContext* opCtx) {
            Impl::checkCanRunHere(opCtx);
            constexpr auto cmdName = Request::kCommandName;
            const auto originalCountRequest = request();

            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            const NamespaceString originalNss(ns());
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace specified '"
                                  << originalNss.toStringForErrorMsg() << "'",
                    originalNss.isValid());

            try {
                sharding::router::CollectionRouter router(opCtx, originalNss);
                return router.routeWithRoutingContext(
                    cmdName, [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                        // Clear the bodyBuilder since this lambda function may be retried if the
                        // router cache is stale.

                        // We will modify this countRequest before sending to shards.
                        auto countRequestForShard = originalCountRequest;

                        // Transform the nss, routingCtx and cmdObj if the 'rawData' field is
                        // enabled and the collection is timeseries.
                        auto nss = originalNss;
                        const auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
                        auto& routingCtx = performTimeseriesTranslationAccordingToRoutingInfo(
                            opCtx,
                            originalNss,
                            targeter,
                            originalRoutingCtx,
                            [&](const NamespaceString& translatedNss) {
                                countRequestForShard.setNamespaceOrUUID(translatedNss);
                                nss = translatedNss;
                            });


                        // Forbid users from passing 'originalQueryShapeHash' explicitly.
                        uassert(10742704,
                                "BSON field 'originalQueryShapeHash' is an unknown field",
                                !countRequestForShard.getOriginalQueryShapeHash().has_value());

                        // Create an RAII object that prints the collection's shard key in the case
                        // of a tassert or crash.
                        const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                        ScopedDebugInfo shardKeyDiagnostics(
                            "ShardKeyDiagnostics",
                            diagnostic_printers::ShardKeyDiagnosticPrinter{
                                cri.isSharded()
                                    ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                    : BSONObj()});

                        const auto collation =
                            countRequestForShard.getCollation().get_value_or(BSONObj());

                        if (prepareForFLERewrite(opCtx,
                                                 countRequestForShard.getEncryptionInformation())) {
                            processFLECountS(opCtx, nss, countRequestForShard);
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
                            "ExpCtxDiagnostics",
                            diagnostic_printers::ExpressionContextPrinter{expCtx});

                        createShapeAndRegisterQueryStats(expCtx, countRequestForShard, nss);

                        // Note: This must happen after query stats because query stats retain the
                        // originating command type for timeseries.
                        {
                            // This scope is used to end the use of the builder whether or not we
                            // convert to a view-less timeseries aggregate request.
                            auto aggResult = BSONObjBuilder{};

                            if (convertAndRunAggregateIfViewlessTimeseries(
                                    opCtx, routingCtx, aggResult, countRequestForShard, nss)) {
                                return count_cmd_helper::buildCountReply(
                                    ViewResponseFormatter{aggResult.obj()}.getCountValue(
                                        boost::none /*tenantId*/));
                            }
                        }

                        // We only need to factor in the skip value when sending to the shards if we
                        // have a value for limit, otherwise, we apply it only once we have
                        // collected all counts.
                        if (countRequestForShard.getLimit() && countRequestForShard.getSkip()) {
                            const auto limit = countRequestForShard.getLimit().value();
                            const auto skip = countRequestForShard.getSkip().value();
                            if (limit != 0) {
                                std::int64_t sum = 0;
                                uassert(ErrorCodes::Overflow,
                                        str::stream()
                                            << "Overflow on the count command: The sum of "
                                               "the limit and skip "
                                               "fields must fit into a long integer. Limit: "
                                            << limit << "   Skip: " << skip,
                                        !overflow::add(limit, skip, &sum));
                                countRequestForShard.setLimit(sum);
                            }
                        }
                        countRequestForShard.setSkip(boost::none);

                        // The includeQueryStatsMetrics field is not supported on mongos for the
                        // count command, so we do not need to check the value on the original
                        // request when updating requestQueryStats here.
                        bool requestQueryStats =
                            query_stats::shouldRequestRemoteMetrics(CurOp::get(opCtx)->debug());

                        std::vector<AsyncRequestsSender::Response> shardResponses;
                        try {
                            shardResponses = scatterGatherVersionedTargetByRoutingTable(
                                expCtx,
                                routingCtx,
                                nss,
                                applyReadWriteConcern(
                                    opCtx,
                                    this,
                                    prepareCountForPassthrough(
                                        opCtx, countRequestForShard.toBSON(), requestQueryStats)),
                                ReadPreferenceSetting::get(opCtx),
                                Shard::RetryPolicy::kIdempotent,
                                countRequestForShard.getQuery(),
                                collation,
                                true /*eligibleForSampling*/);

                        } catch (const ExceptionFor<
                                 ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                            // Rewrite the count command as an aggregation.
                            auto aggRequestOnView =
                                query_request_conversion::asAggregateCommandRequest(
                                    originalCountRequest);

                            const ResolvedView& resolvedView = *ex.extraInfo<ResolvedView>();
                            auto resolvedAggRequest =
                                PipelineResolver::buildRequestWithResolvedPipeline(
                                    expCtx->getIfrContext(), resolvedView, aggRequestOnView);

                            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                                opCtx,
                                OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                                            originalCountRequest.getDbName(),
                                                            resolvedAggRequest.toBSON()));

                            return count_cmd_helper::buildCountReply(
                                ViewResponseFormatter{aggResult}.getCountValue(
                                    boost::none /*tenantId*/));
                        }

                        long long total = 0;
                        bool allShardMetricsReturned = true;
                        BSONObjBuilder shardsSubTotalBob;

                        for (const auto& response : shardResponses) {
                            auto status = response.swResponse.getStatus();
                            if (status.isOK()) {
                                status =
                                    getStatusFromCommandResult(response.swResponse.getValue().data);
                                if (status.isOK()) {
                                    const BSONObj& data = response.swResponse.getValue().data;
                                    const long long shardCount = data["n"].numberLong();
                                    shardsSubTotalBob.appendNumber(response.shardId.toString(),
                                                                   shardCount);
                                    total += shardCount;

                                    // We aggregate the metrics from all the shards. If any shard
                                    // does not include metrics, we avoid collecting the remote
                                    // metrics for the entire query and do not write an entry to the
                                    // query stats store. Note that we do not expect shards to
                                    // fail
                                    // to collect metrics for the count command; this is just
                                    // thorough error handling.
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

                            // Add error context so that you can see on which shard failed as well
                            // as details about that error.
                            uassertStatusOK(status.withContext(str::stream() << "failed on: "
                                                                             << response.shardId));
                        }

                        const auto shardsObj = shardsSubTotalBob.obj();
                        // Use skip and limit from the originalCountRequest when calculating the
                        // total count.
                        total = applySkipLimit(
                            total, originalCountRequest.getSkip(), originalCountRequest.getLimit());

                        Reply reply = count_cmd_helper::buildCountReply(total);
                        reply.setShards(shardsObj);

                        // The # of documents returned is always 1 for the count command.
                        static constexpr long long kNReturned = 1;

                        auto* curOp = CurOp::get(opCtx);
                        curOp->setEndOfOpMetrics(kNReturned);

                        if (allShardMetricsReturned) {
                            collectQueryStatsMongos(
                                opCtx, std::move(curOp->debug().getQueryStatsInfo().key));
                        }
                        return reply;
                    });
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // If there's no collection with this name, the count aggregation behavior below
                // will produce a total count of 0.
                Reply reply = count_cmd_helper::buildCountReply(0);

                // The # of documents returned is always 1 for the count command.
                auto* curOp = CurOp::get(opCtx);
                curOp->setEndOfOpMetrics(1);

                collectQueryStatsMongos(opCtx, std::move(curOp->debug().getQueryStatsInfo().key));
                return reply;
            }
        }

    private:
        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* reply) override {
            Impl::checkCanExplainHere(opCtx);

            const auto originalCountRequest = request();
            BSONObjBuilder result;
            auto curOp = CurOp::get(opCtx);
            curOp->debug().getQueryStatsInfo().disableForSubqueryExecution = true;

            const NamespaceString originalNss(ns());
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace specified '"
                                  << originalNss.toStringForErrorMsg() << "'",
                    originalNss.isValid());

            sharding::router::CollectionRouter router(opCtx, originalNss);
            auto status = router.routeWithRoutingContext(
                "explain count"_sd,
                [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                    // Clear the bodyBuilder since this lambda function may be retried if the router
                    // cache is stale.
                    result.resetToEmpty();

                    // Transform the nss, routingCtx and cmdObj if the 'rawData' field is enabled
                    // and the collection is timeseries.
                    auto countRequestForShard = originalCountRequest;
                    const auto& cmdObj = unparsedRequest().body;
                    auto nss = originalNss;
                    const auto targeter = CollectionRoutingInfoTargeter(opCtx, originalNss);
                    auto& routingCtx = performTimeseriesTranslationAccordingToRoutingInfo(
                        opCtx,
                        originalNss,
                        targeter,
                        originalRoutingCtx,
                        [&](const NamespaceString& translatedNss) {
                            countRequestForShard.setNamespaceOrUUID(translatedNss);
                            nss = translatedNss;
                        });


                    // Create an RAII object that prints the collection's shard key in the case of a
                    // tassert or crash.
                    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                    ScopedDebugInfo shardKeyDiagnostics(
                        "ShardKeyDiagnostics",
                        diagnostic_printers::ShardKeyDiagnosticPrinter{
                            cri.isSharded() ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                            : BSONObj()});

                    {
                        // This scope is used to end the use of the builder whether or not we
                        // convert to a view-less timeseries aggregate request.
                        if (convertAndRunAggregateIfViewlessTimeseries(
                                opCtx, routingCtx, result, countRequestForShard, nss, verbosity)) {
                            // We've delegated execution to agg.
                            return Status::OK();
                        }
                    }

                    // If the command has encryptionInformation, rewrite the query as necessary.
                    if (prepareForFLERewrite(opCtx,
                                             countRequestForShard.getEncryptionInformation())) {
                        processFLECountS(opCtx, nss, countRequestForShard);
                    }

                    BSONObj targetingQuery = countRequestForShard.getQuery();
                    BSONObj targetingCollation =
                        countRequestForShard.getCollation().value_or(BSONObj());

                    auto expCtx = makeBlankExpressionContext(opCtx, nss);

                    // Compute QueryShapeHash and record it in CurOp for explain output.
                    const std::unique_ptr<ParsedFindCommand> parsedFind =
                        uassertStatusOK(parsed_find_command::parseFromCount(
                            expCtx, countRequestForShard, ExtensionsCallbackNoop(), nss));

                    const query_shape::DeferredQueryShape deferredShape{[&]() {
                        return shape_helpers::tryMakeShape<query_shape::CountCmdShape>(
                            *parsedFind,
                            countRequestForShard.getLimit().has_value(),
                            countRequestForShard.getSkip().has_value());
                    }};

                    CurOp::get(opCtx)->debug().ensureQueryShapeHash(opCtx, [&]() {
                        return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nss);
                    });

                    auto numShards =
                        getTargetedShardsForQuery(expCtx, cri, targetingQuery, targetingCollation)
                            .size();
                    auto userLimit = countRequestForShard.getLimit();
                    auto userSkip = countRequestForShard.getSkip();
                    if (numShards > 1) {
                        // If there is a limit, we forward the sum of the limit and skip.
                        auto swNewLimit = addLimitAndSkipForShards(countRequestForShard.getLimit(),
                                                                   countRequestForShard.getSkip());
                        if (!swNewLimit.isOK()) {
                            return swNewLimit.getStatus();
                        }
                        countRequestForShard.setLimit(swNewLimit.getValue());
                        countRequestForShard.setSkip(boost::none);
                    }

                    const auto explainCmd =
                        ClusterExplain::wrapAsExplain(countRequestForShard.toBSON(), verbosity);

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
                        const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&
                            ex) {
                        auto aggRequestOnView = query_request_conversion::asAggregateCommandRequest(
                            originalCountRequest, true /* hasExplain */);
                        // An empty PrivilegeVector is acceptable because these privileges are only
                        // checked on getMore and explain will not open a cursor.
                        return ClusterAggregate::retryOnViewOrIFRKickbackError(
                            opCtx,
                            aggRequestOnView,
                            *ex.extraInfo<ResolvedView>(),
                            nss,
                            PrivilegeVector(),
                            verbosity,
                            &result);
                    }

                    long long millisElapsed = timer.millis();

                    const char* mongosStageName = ClusterExplain::getStageNameForReadOp(
                        shardResponses.size(), countRequestForShard);

                    return ClusterExplain::buildExplainResult(expCtx,
                                                              shardResponses,
                                                              mongosStageName,
                                                              millisElapsed,
                                                              cmdObj,
                                                              &result,
                                                              userLimit,
                                                              userSkip);
                });
            uassertStatusOK(status);
            auto bodyBuilder = reply->getBodyBuilder();
            bodyBuilder.appendElements(result.obj());
        }
        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            static const Status kSnapshotNotSupported{ErrorCodes::InvalidOptions,
                                                      "read concern snapshot not supported"};
            return {{level == repl::ReadConcernLevel::kSnapshotReadConcern, kSnapshotNotSupported},
                    Status::OK()};
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        bool supportsRawData() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            if (!as->isAuthorizedForActionsOnResource(
                    CommandHelpers::resourcePatternForNamespace(ns()), ActionType::find)) {
                uasserted(ErrorCodes::Unauthorized, "unauthorized");
            }

            auto status = Impl::checkAuthForOperation(opCtx, request().getDbName(), request());
            uassertStatusOK(status);
        }

        NamespaceString ns() const override {
            const auto& nsOrUUID = request().getNamespaceOrUUID();
            uassert(ErrorCodes::InvalidNamespace,
                    "clusterCount expected namespace-string, not UUID",
                    nsOrUUID.isNamespaceString());
            return nsOrUUID.nss();
        }
    };

private:
    static long long applySkipLimit(long long num,
                                    boost::optional<std::int64_t> skip,
                                    boost::optional<std::int64_t> limit) {

        if (skip.has_value()) {
            num = num - *skip;
            if (num < 0) {
                num = 0;
            }
        }

        if (limit.has_value()) {
            auto currLimit = *limit;
            if (currLimit < 0) {
                currLimit = -currLimit;
            }

            // 0 limit means no limit
            if (currLimit < num && currLimit != 0) {
                num = currLimit;
            }
        }

        return num;
    }
};

}  // namespace mongo
