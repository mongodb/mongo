/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/views/pipeline_resolver.h"

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/views/resolved_view.h"

namespace mongo {
namespace {
/**
 * Builds the base resolved request by copying the original request, setting the namespace to the
 * resolved view's namespace, and handling explain/cursor precedence.
 */
AggregateCommandRequest buildBaseResolvedRequest(const ResolvedView& resolvedView,
                                                 const AggregateCommandRequest& originalRequest) {
    // Start with a copy of the original request and modify fields as needed. We assume that most
    // fields should be unchanged from the original request; any fields that need to be changed will
    // be modified below.
    // TODO SERVER-110454: Avoid copying the original pipeline when possible.
    AggregateCommandRequest resolvedRequest = originalRequest;
    resolvedRequest.setNamespace(resolvedView.getNamespace());

    // If both 'explain' and 'cursor' are set, we give precedence to 'explain' and drop 'cursor'.
    if (originalRequest.getExplain()) {
        resolvedRequest.setCursor(SimpleCursorOptions());
    }

    return resolvedRequest;
}

/**
 * Builds a resolved pipeline by prepending the view pipeline to the
 * user pipeline. Handles special cases for mongot pipelines and timeseries views.
 * For mongot pipelines, returns just the user pipeline (view pipeline is applied by
 * $_internalSearchIdLookup). For timeseries views, prepends the view pipeline.
 */
std::vector<BSONObj> buildResolvedPipelineForSimpleCase(const ResolvedView& resolvedView,
                                                        const std::vector<BSONObj>& userPipeline) {
    // Mongot user pipelines are a unique case: $_internalSearchIdLookup applies the view pipeline.
    // For this reason, we do not expand the aggregation request to include the view pipeline.
    if (search_helper_bson_obj::isMongotPipeline(userPipeline)) {
        return userPipeline;
    }

    // The new pipeline consists of two parts: first, 'pipeline' in this ResolvedView; then, the
    // pipeline in 'request'.
    auto& viewPipeline = resolvedView.getPipeline();
    std::vector<BSONObj> resolvedPipeline;
    resolvedPipeline.reserve(viewPipeline.size() + userPipeline.size());
    resolvedPipeline.insert(resolvedPipeline.end(), viewPipeline.begin(), viewPipeline.end());
    resolvedPipeline.insert(resolvedPipeline.end(), userPipeline.begin(), userPipeline.end());
    return resolvedPipeline;
}

/**
 * Builds the resolved pipeline for regular views (non-mongot, non-timeseries). This requires
 * mongos-specific desugaring and serialization to ensure view info is properly bound and included
 * in the BSON sent from mongos to the shards.
 */
std::pair<std::vector<BSONObj>, boost::optional<LiteParsedPipeline>>
buildResolvedPipelineForRegularView(OperationContext* opCtx,
                                    const AggregateCommandRequest& request,
                                    const ResolvedView& resolvedView,
                                    const NamespaceString& requestedNss,
                                    boost::optional<ExplainOptions::Verbosity> verbosity,
                                    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
                                    const PipelineResolver::MongosPipelineHelpers& helpers) {
    // For regular views, we need mongos-specific desugaring and serialization to ensure
    // view info is properly bound and included in the BSON sent from mongos to the shards.
    auto lpp = LiteParsedPipeline(request, true, LiteParserOptions{.ifrContext = ifrContext});
    lpp.makeOwned();

    // Desugar and call handleView() to invoke ViewPolicy callbacks for extension stages.
    LiteParsedDesugarer::desugar(&lpp);

    PipelineResolver::applyViewToLiteParsed(
        &lpp, resolvedView, requestedNss, LiteParserOptions{.ifrContext = ifrContext});

    // Parse the modified LiteParsedPipeline to a full Pipeline and serialize the Pipeline back
    // to BSON. This ensures that any view info bound to LiteParsedDocumentSources is included
    // in the BSON sent from mongos to the shards.
    auto expCtx = helpers.makeExpressionContext(
        opCtx,
        request,
        boost::none,  // cri
        resolvedView.getNamespace(),
        requestedNss,
        resolvedView.getDefaultCollation(),
        boost::none,  // uuid
        helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces()),
        false,  // hasChangeStream
        verbosity,
        ExpressionContextCollationMatchesDefault::kYes,
        ifrContext);
    auto pipeline =
        Pipeline::parseFromLiteParsed(lpp, expCtx, nullptr, false, true /* useStubInterface */);

    return {pipeline->serializeToBson(), std::move(lpp)};
}

/**
 * Applies final transformations to a resolved request:
 * - Applies timeseries rewrites if needed
 * - Rewrites index hints for timeseries views
 * - Sets the view's default collation
 */
void applyFinalTransformations(AggregateCommandRequest& resolvedRequest,
                               std::vector<BSONObj>&& resolvedPipeline,
                               const ResolvedView& resolvedView,
                               const AggregateCommandRequest& originalRequest) {
    // Apply timeseries rewrites if the first stage is $_internalUnpackBucket.
    if (resolvedPipeline.size() >= 1 &&
        resolvedPipeline[0][DocumentSourceInternalUnpackBucket::kStageNameInternal]) {
        resolvedView.applyTimeseriesRewrites(&resolvedPipeline);
    }

    // Set the pipeline once after all transformations are complete.
    resolvedRequest.setPipeline(std::move(resolvedPipeline));

    // If we have an index hint on a time-series view, we may need to rewrite the index spec to
    // match the index on the underlying buckets collection.
    if (originalRequest.getHint() && resolvedView.timeseries()) {
        auto newHint = resolvedView.rewriteIndexHintForTimeseries(*originalRequest.getHint());
        if (newHint.has_value()) {
            resolvedRequest.setHint(*newHint);
        }
    }

    // Operations on a view must always use the default collation of the view. We must have already
    // checked that if the user's request specifies a collation, it matches the collation of the
    // view.
    resolvedRequest.setCollation(resolvedView.getDefaultCollation());
}
}  // namespace

AggregateCommandRequest PipelineResolver::buildRequestWithResolvedPipeline(
    const ResolvedView& resolvedView, const AggregateCommandRequest& originalRequest) {
    AggregateCommandRequest expandedRequest =
        buildBaseResolvedRequest(resolvedView, originalRequest);

    std::vector<BSONObj> resolvedPipeline =
        buildResolvedPipelineForSimpleCase(resolvedView, originalRequest.getPipeline());

    applyFinalTransformations(
        expandedRequest, std::move(resolvedPipeline), resolvedView, originalRequest);

    return expandedRequest;
}

void PipelineResolver::applyViewToLiteParsed(LiteParsedPipeline* userLPP,
                                             const ResolvedView& resolvedView,
                                             const NamespaceString& viewNss,
                                             const LiteParserOptions& options) {
    // Desugar the viewPipeline and apply it to the user pipeline.
    auto viewInfo = resolvedView.toViewInfo(viewNss, options);
    LiteParsedDesugarer::desugar(viewInfo.viewPipeline.get());
    userLPP->handleView(viewInfo);
}

PipelineResolver::MongosViewRequestResult PipelineResolver::buildResolvedMongosViewRequest(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const ResolvedView& resolvedView,
    const NamespaceString& requestedNss,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
    const MongosPipelineHelpers& helpers) {
    // Build the base resolved request (copy, set namespace, handle explain/cursor).
    AggregateCommandRequest resolvedAggRequest = buildBaseResolvedRequest(resolvedView, request);

    // Build the resolved pipeline. Mongot and timeseries views can use the simple pipeline
    // building logic. Regular views require mongos-specific desugaring and serialization.
    std::vector<BSONObj> resolvedPipeline;
    boost::optional<LiteParsedPipeline> userLPP;
    if (search_helper_bson_obj::isMongotPipeline(request.getPipeline()) ||
        resolvedView.timeseries()) {
        resolvedPipeline = buildResolvedPipelineForSimpleCase(resolvedView, request.getPipeline());
        userLPP = boost::none;
    } else {
        std::tie(resolvedPipeline, userLPP) = buildResolvedPipelineForRegularView(
            opCtx, request, resolvedView, requestedNss, verbosity, ifrContext, helpers);
    }

    // Apply final transformations (timeseries rewrites, index hint rewriting, collation setting).
    applyFinalTransformations(
        resolvedAggRequest, std::move(resolvedPipeline), resolvedView, request);

    return MongosViewRequestResult{std::move(resolvedAggRequest), std::move(userLPP)};
}

}  // namespace mongo
