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
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {
namespace {
/**
 * Builds the base resolved request by copying the original request, setting the namespace to the
 * resolved view's namespace (if any), and handling explain/cursor precedence. When 'resolvedView'
 * is boost::none the namespace is left unchanged (top-level is a base collection).
 */
AggregateCommandRequest buildBaseResolvedRequest(const boost::optional<ResolvedView>& resolvedView,
                                                 const AggregateCommandRequest& originalRequest) {
    // Start with a copy of the original request and modify fields as needed. We assume that most
    // fields should be unchanged from the original request; any fields that need to be changed will
    // be modified below.
    // TODO SERVER-110454: Avoid copying the original pipeline when possible.
    AggregateCommandRequest resolvedRequest = originalRequest;
    if (resolvedView) {
        resolvedRequest.setNamespace(resolvedView->getNamespace());
    }

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
std::vector<BSONObj> buildResolvedPipelineForSimpleCase(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const ResolvedView& resolvedView,
    const std::vector<BSONObj>& userPipeline) {
    // Mongot user pipelines are a unique case: $_internalSearchIdLookup applies the view pipeline.
    // For this reason, we do not expand the aggregation request to include the view pipeline.
    // Caller is expected to use LiteParsedPipeline::handleView() for such cases.
    if (search_helper_bson_obj::isMongotPipeline(ifrContext, userPipeline) ||
        search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, userPipeline)) {
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
 * Builds the resolved pipeline for the regular (non-mongot, non-timeseries) path. Handles both:
 *   - 'resolvedView' set: the top-level namespace is a view; insert a top-level view entry
 *     so its pipeline is prepended.
 *   - 'resolvedView' boost::none: the top-level namespace is a base collection; the closure in
 *     'preResolvedNamespaces' is still threaded into the LiteParsedPipeline so subpipeline
 *     views (e.g. inside $unionWith / $lookup) get their stages stitched.
 *
 * In both cases this requires mongos-specific desugaring and serialization to ensure view info
 * is properly bound and included in the BSON sent from mongos to the shards.
 */
std::pair<std::vector<BSONObj>, boost::optional<LiteParsedPipeline>>
buildResolvedPipelineForRegularView(OperationContext* opCtx,
                                    const AggregateCommandRequest& request,
                                    const boost::optional<ResolvedView>& resolvedView,
                                    const NamespaceString& requestedNss,
                                    boost::optional<ExplainOptions::Verbosity> verbosity,
                                    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
                                    const PipelineResolver::MongosPipelineHelpers& helpers,
                                    const ResolvedNamespaceMap& preResolvedNamespaces) {
    // For regular views, we need mongos-specific desugaring and serialization to ensure
    // view info is properly bound and included in the BSON sent from mongos to the shards.
    auto lpp = LiteParsedPipeline(request, true, LiteParserOptions{.ifrContext = ifrContext});
    lpp.makeOwned();

    // Desugar and call handleView() to invoke bindViewInfo() for extension stages.
    LiteParsedDesugarer::desugar(&lpp, ifrContext);

    auto mergeResolved = [&](ResolvedNamespaceMap m) {
        for (const auto& [nss, rn] : preResolvedNamespaces) {
            m.insert_or_assign(nss, rn);
        }
        return m;
    };

    auto resolvedNamespaces =
        mergeResolved(helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces()));

    if (resolvedView) {
        PipelineResolver::insertTopLevelViewEntry(
            resolvedNamespaces, requestedNss, *resolvedView, ifrContext);
    }

    const bool userHadSearchStage = lpp.hasSearchStage();

    PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &lpp, requestedNss, resolvedNamespaces);
    // Involved namespaces may have been added to lpp, so get the most up to date set.
    auto updatedResolvedNamespaces =
        mergeResolved(helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces()));

    // Parse the modified LiteParsedPipeline to a full Pipeline and serialize the Pipeline back
    // to BSON. This ensures that any view info bound to LiteParsedDocumentSources is included
    // in the BSON sent from mongos to the shards. When the top-level is a base collection, fall
    // back to the request's own namespace and collation.
    const auto& executionNss = resolvedView ? resolvedView->getNamespace() : request.getNamespace();
    BSONObj collationObj = resolvedView ? resolvedView->getDefaultCollation()
                                        : request.getCollation().value_or(BSONObj{});
    auto expCtx = helpers.makeExpressionContext(opCtx,
                                                request,
                                                boost::none,  // cri
                                                executionNss,
                                                requestedNss,
                                                collationObj,
                                                boost::none,  // uuid
                                                updatedResolvedNamespaces,
                                                false,  // hasChangeStream
                                                verbosity,
                                                ExpressionContextCollationMatchesDefault::kYes,
                                                ifrContext);
    // For $rankFusion/$scoreFusion on views, set view on expCtx so $search/$vectorSearch inside
    // them embed the view pipeline during desugaring (their LiteParsed stage returns false from
    // shouldResolveSubpipelineViews, so expCtx->getView() is the fallback).
    // TODO SERVER-121094 Stopgap until the legacy $search path is removed.
    if (resolvedView && userHadSearchStage) {
        search_helpers::checkAndSetViewOnExpCtx(expCtx, lpp, *resolvedView, requestedNss);
    }
    auto pipeline =
        Pipeline::parseFromLiteParsed(lpp, expCtx, nullptr, false, true /* useStubInterface */);

    query_shape::SerializationOptions wireOpts{.isSerializingForRemoteDispatch = true};
    return {pipeline->serializeToBson(wireOpts), std::move(lpp)};
}

/**
 * Applies final transformations to a resolved request:
 * - Applies timeseries rewrites if needed (only when 'resolvedView' is set)
 * - Rewrites index hints for timeseries views (only when 'resolvedView' is set)
 * - Sets the view's default collation (only when 'resolvedView' is set)
 *
 * When 'resolvedView' is boost::none the top-level namespace is a base collection, so the
 * request's namespace, hint, and collation are left untouched.
 */
void applyFinalTransformations(AggregateCommandRequest& resolvedRequest,
                               std::vector<BSONObj>&& resolvedPipeline,
                               const boost::optional<ResolvedView>& resolvedView,
                               const AggregateCommandRequest& originalRequest) {
    // Apply timeseries rewrites if the first stage is $_internalUnpackBucket.
    if (resolvedView && !resolvedPipeline.empty() &&
        resolvedPipeline[0][DocumentSourceInternalUnpackBucket::kStageNameInternal]) {
        resolvedView->applyTimeseriesRewrites(&resolvedPipeline);
    }

    // Set the pipeline once after all transformations are complete.
    resolvedRequest.setPipeline(std::move(resolvedPipeline));

    if (!resolvedView) {
        return;
    }

    // If we have an index hint on a time-series view, we may need to rewrite the index spec to
    // match the index on the underlying buckets collection.
    if (originalRequest.getHint() && resolvedView->isTimeseries()) {
        auto newHint = resolvedView->rewriteIndexHintForTimeseries(*originalRequest.getHint());
        if (newHint.has_value()) {
            resolvedRequest.setHint(*newHint);
        }
    }

    // Operations on a view must always use the default collation of the view. We must have already
    // checked that if the user's request specifies a collation, it matches the collation of the
    // view.
    resolvedRequest.setCollation(resolvedView->getDefaultCollation());
}
}  // namespace

AggregateCommandRequest PipelineResolver::buildRequestWithResolvedPipeline(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const ResolvedView& resolvedView,
    const AggregateCommandRequest& originalRequest) {
    AggregateCommandRequest expandedRequest =
        buildBaseResolvedRequest(resolvedView, originalRequest);

    std::vector<BSONObj> resolvedPipeline =
        buildResolvedPipelineForSimpleCase(ifrContext, resolvedView, originalRequest.getPipeline());

    applyFinalTransformations(
        expandedRequest, std::move(resolvedPipeline), resolvedView, originalRequest);

    return expandedRequest;
}

void PipelineResolver::applyViewToLiteParsed(LiteParsedPipeline* userLPP,
                                             const ResolvedView& resolvedView,
                                             const NamespaceString& viewNss,
                                             const ResolvedNamespaceMap& resolvedNamespaces,
                                             const LiteParserOptions& options) {
    // Desugar extension stages in the view pipeline before stitching. This uses a callback
    // registered by lite_parsed_desugarer.cpp to avoid a circular build dependency between
    // ResolvedNamespace and LiteParsedDesugarer.
    auto viewInfo = resolvedView.toViewInfo(viewNss, options);
    viewInfo.desugarViewPipeline();
    userLPP->handleView(viewInfo, resolvedNamespaces);
}

void PipelineResolver::validateStagesOnView(LiteParsedPipeline* userLPP,
                                            const ResolvedView& resolvedView,
                                            const NamespaceString& viewNss,
                                            const ResolvedNamespaceMap& resolvedNamespaces,
                                            const LiteParserOptions& options) {
    auto viewInfo = resolvedView.toViewInfo(viewNss, options);
    viewInfo.desugarViewPipeline();
    userLPP->bindViewInfoToStages(viewInfo, resolvedNamespaces);
}

PipelineResolver::MongosViewRequestResult PipelineResolver::buildResolvedMongosViewRequest(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const boost::optional<ResolvedView>& resolvedView,
    const NamespaceString& requestedNss,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
    const MongosPipelineHelpers& helpers,
    const ResolvedNamespaceMap& preResolvedNamespaces) {
    // Build the base resolved request (copy, set namespace if there's a top-level view, handle
    // explain/cursor).
    AggregateCommandRequest resolvedAggRequest = buildBaseResolvedRequest(resolvedView, request);

    // Build the resolved pipeline. Mongot and timeseries views can use the simple pipeline
    // building logic. Regular views and the no-top-level-view case (where the closure in
    // 'preResolvedNamespaces' must still be threaded through subpipeline lite-parsing) go through
    // the regular path.
    std::vector<BSONObj> resolvedPipeline;
    boost::optional<LiteParsedPipeline> userLPP;
    if (resolvedView &&
        !search_helper_bson_obj::isHybridSearchBsonPipeline(request.getPipeline()) &&
        (search_helper_bson_obj::isMongotPipeline(ifrContext, request.getPipeline()) ||
         resolvedView->isTimeseries())) {
        resolvedPipeline =
            buildResolvedPipelineForSimpleCase(ifrContext, *resolvedView, request.getPipeline());

        // For mongot pipelines on views, validate that extension stages are allowed. The legacy
        // first stage handles view resolution itself, but subsequent extension stages still need
        // view validation. Timeseries views are skipped; their pipeline is fully resolved above via
        // buildResolvedPipelineForSimpleCase.
        if (!resolvedView->isTimeseries()) {
            LiteParserOptions options{.ifrContext = ifrContext};
            auto lpp = LiteParsedPipeline(request, true, options);
            lpp.makeOwned();
            LiteParsedDesugarer::desugar(&lpp, ifrContext);
            auto resolvedNamespaces =
                helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces());
            validateStagesOnView(&lpp, *resolvedView, requestedNss, resolvedNamespaces, options);
        }

        userLPP = boost::none;
    } else {
        std::tie(resolvedPipeline, userLPP) =
            buildResolvedPipelineForRegularView(opCtx,
                                                request,
                                                resolvedView,
                                                requestedNss,
                                                verbosity,
                                                ifrContext,
                                                helpers,
                                                preResolvedNamespaces);
    }

    // Apply final transformations (timeseries rewrites, index hint rewriting, collation setting).
    // No-ops when 'resolvedView' is null.
    applyFinalTransformations(
        resolvedAggRequest, std::move(resolvedPipeline), resolvedView, request);

    return MongosViewRequestResult{std::move(resolvedAggRequest), std::move(userLPP)};
}

namespace {
bool resolveInvolvedNamespacesImpl(LiteParsedPipeline* lpp,
                                   const NamespaceString& mainNss,
                                   const ResolvedNamespaceMap& resolvedNamespaces,
                                   stdx::unordered_set<NamespaceString>& inProgress) {
    // Build the ViewInfo to hand to handleView. If 'mainNss' is a view in the map, build a real
    // ViewInfo from it and desugar its pipeline, else pass a sentinel ViewInfo.
    bool anyViewBound = false;
    ViewInfo viewInfo;
    if (auto it = resolvedNamespaces.find(mainNss);
        it != resolvedNamespaces.end() && it->second.involvedNamespaceIsAView) {
        // TODO SERVER-122116 Clean this up when ViewInfo is fully replaced by ResolvedNamespace.
        ResolvedNamespace viewEntry = it->second;
        viewEntry.liteParseViewPipeline();
        viewInfo = ViewInfo(viewEntry);
        viewInfo.desugarViewPipeline();
        anyViewBound = true;
    }

    // Capture the original stage count BEFORE handleView/_stitchFront prepends the view's stages.
    // The delta (post - pre) is the number of newly prepended stages iterated in Pass A below.
    const size_t originalSize = lpp->getStages().size();

    if (search_helpers::isMongotLiteParsedPipeline(*lpp)) {
        lpp->bindViewInfoToStages(viewInfo, resolvedNamespaces);
    } else {
        lpp->handleView(viewInfo, resolvedNamespaces);
    }

    // NOTE: lpp->getStages() must be obtained AFTER handleView/_stitchFront, which replaces
    // the internal vector, invalidating any reference taken before the call.
    const auto& stages = lpp->getStages();
    const size_t viewStageCount = stages.size() - originalSize;

    // Pass A: walk stages handleView/_stitchFront just prepended (indices [0, viewStageCount)).
    // These came from the view's own definition, so a subpipeline back to mainNss is a true
    // cycle. Our caller inserted mainNss into inProgress, so the guard below suppresses it.
    for (size_t i = 0; i < viewStageCount; ++i) {
        if (!stages[i]->shouldResolveSubpipelineViews()) {
            continue;
        }
        auto* subs = stages[i]->getMutableSubPipelines();
        if (!subs) {
            continue;
        }
        for (auto& sub : *subs) {
            auto subNss = sub->getOriginalParseNss();
            // Cycle detection: if subNss is already being processed somewhere up the recursion
            // stack, skip it. Without this guard mutually-referencing view definitions (each
            // pointing into the other and back to a common base collection) would recurse
            // indefinitely.
            if (!inProgress.insert(subNss).second) {
                continue;
            }
            anyViewBound |= resolveInvolvedNamespacesImpl(
                sub.operator->(), subNss, resolvedNamespaces, inProgress);
            inProgress.erase(subNss);
        }
    }

    // Pass B: walk stages that were already in the LPP before handleView prepended the view
    // definition (indices [viewStageCount, N)). Whether a subpipeline reference back to mainNss
    // is a legitimate user query or an infinite loop depends on whether mainNss is a view.
    //
    // Case A — mainNss is a view (anyViewBound=true): the stages here are user-written. A user
    // query may legitimately reference the same view again (e.g. nested $lookup depth checks).
    // See PipelineResolverTest::AllowsRepeatedViewReferenceInUserStages for a unit test.
    //
    // Case B — mainNss is a base collection (anyViewBound=false): the stages here were planted
    // by view resolution. Any back-reference indicates a cycle.
    // See jstests/concurrency/fsm_workloads/view_catalog/view_catalog_cycle_lookup.js for an
    // end-to-end test of the $graphLookup mutual-view-cycle this guards against.
    const bool wasInProgress = anyViewBound && (inProgress.erase(mainNss) > 0);
    for (size_t i = viewStageCount; i < stages.size(); ++i) {
        if (!stages[i]->shouldResolveSubpipelineViews()) {
            continue;
        }
        auto* subs = stages[i]->getMutableSubPipelines();
        if (!subs) {
            continue;
        }
        for (auto& sub : *subs) {
            auto subNss = sub->getOriginalParseNss();
            if (!inProgress.insert(subNss).second) {
                continue;
            }
            anyViewBound |= resolveInvolvedNamespacesImpl(
                sub.operator->(), subNss, resolvedNamespaces, inProgress);
            inProgress.erase(subNss);
        }
    }
    if (wasInProgress) {
        inProgress.insert(mainNss);
    }

    return anyViewBound;
}
}  // namespace

bool PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
    LiteParsedPipeline* lpp,
    const NamespaceString& mainNss,
    const ResolvedNamespaceMap& resolvedNamespaces) {
    stdx::unordered_set<NamespaceString> inProgress;
    return resolveInvolvedNamespacesImpl(lpp, mainNss, resolvedNamespaces, inProgress);
}

void PipelineResolver::insertTopLevelViewEntry(
    ResolvedNamespaceMap& resolvedNamespaces,
    const NamespaceString& requestedNss,
    const ResolvedView& resolvedView,
    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext) {
    ResolvedNamespaceViewOptions viewOptions;
    viewOptions.involvedNamespaceIsAView = true;
    viewOptions.shouldParseLpp = true;
    // Thread the IFR context through so that extension stages within the view pipeline have the
    // same feature-flag view as the top-level request. Without this, extension stages in view
    // definitions lite-parse with a null _ifrContext and LiteParsedExpanded::bindViewInfo trips
    // its hybridSearchFlagEnabled check at view-binding time.
    viewOptions.options =
        std::make_shared<LiteParserOptions>(LiteParserOptions{.ifrContext = std::move(ifrContext)});

    // Preserve the UUID of the underlying collection if the namespace was already in the map
    // from a prior catalog lookup. This could occur when the requested Nss is both the top-level
    // view and targeted by a subpipeline stage.
    if (auto it = resolvedNamespaces.find(requestedNss); it != resolvedNamespaces.end()) {
        viewOptions.collUUID = it->second.uuid;
    }
    resolvedNamespaces.insert_or_assign(requestedNss,
                                        ResolvedNamespace(requestedNss,
                                                          resolvedView.getNamespace(),
                                                          resolvedView.getPipeline(),
                                                          resolvedView.getDefaultCollation(),
                                                          viewOptions));
}
}  // namespace mongo
