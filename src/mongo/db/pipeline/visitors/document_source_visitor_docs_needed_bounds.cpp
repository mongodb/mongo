/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry_mongod.h"
#include "mongo/db/pipeline/visitors/document_source_walker.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/overflow_arithmetic.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
using NeedAll = docs_needed_bounds::NeedAll;
using Unknown = docs_needed_bounds::Unknown;

namespace {
void extractDocsNeededBoundsHelper(const Pipeline& pipeline, DocsNeededBoundsContext* ctx) {
    ServiceContext* serviceCtx = pipeline.getContext()->getOperationContext()->getServiceContext();
    auto& reg = getDocumentSourceVisitorRegistry(serviceCtx);
    DocumentSourceWalker walker(reg, ctx);

    walker.reverseWalk(pipeline);

    tassert(8673200,
            "If one of min/max docs needed bounds is NeedAll, they must both be NeedAll.",
            std::holds_alternative<NeedAll>(ctx->minBounds) ==
                std::holds_alternative<NeedAll>(ctx->maxBounds));
}
}  // namespace

void DocsNeededBoundsContext::applyPossibleDecreaseStage() {
    // If we have existing discrete maxBounds, this stage may reduce the number of documents in the
    // result stream before applying that limit, so we may need to scan more documents than the
    // bounds suggest, to compensate for that decrease. This doesn't affect the minimum number
    // of documents needed (minBounds).
    // For example, in the sequence of a $match before a $limit, without knowing the selectivity of
    // the $match, we must reset the maxBounds to Unknown.
    maxBounds = visit(OverloadedVisitor{
                          [](long long) -> DocsNeededConstraint { return Unknown(); },
                          [](NeedAll all) -> DocsNeededConstraint { return all; },
                          [](Unknown unknown) -> DocsNeededConstraint { return unknown; },
                      },
                      maxBounds);
}

void DocsNeededBoundsContext::applyPossibleIncreaseStage() {
    // If we have existing discrete minBounds, this stage may increase the number of documents in
    // the result stream before applying that limit, meaning we may need to scan even fewer
    // documents than the bounds suggest. This doesn't affect the maximum number of documents needed
    // (maxBounds).
    // For example, in the sequence of a $densify before a limit, we must reset the minBounds to
    // Unknown since we don't know the rate at which $densify will add documents to the stream.
    minBounds = visit(OverloadedVisitor{
                          [](long long) -> DocsNeededConstraint { return Unknown(); },
                          [](NeedAll all) -> DocsNeededConstraint { return all; },
                          [](Unknown unknown) -> DocsNeededConstraint { return unknown; },
                      },
                      minBounds);
}

void DocsNeededBoundsContext::applySkip(long long newSkip) {
    // We apply $skip identically to maxBounds and minBounds, by adding the skip value if it's a
    // discretely $limit-ed bounds, or ignoring it otherwise.
    auto applySkip = [newSkip](DocsNeededConstraint currBounds) -> DocsNeededConstraint {
        return visit(OverloadedVisitor{
                         [newSkip](long long discreteBounds) -> DocsNeededConstraint {
                             long long safeSum = 0;
                             if (overflow::add(discreteBounds, newSkip, &safeSum)) {
                                 // If we're skipping so many that it overflows 64
                                 // bits, we'll consider it as if we just need all
                                 // documents.
                                 return NeedAll();
                             }
                             return safeSum;
                         },
                         [](NeedAll all) -> DocsNeededConstraint { return all; },
                         [](Unknown unknown) -> DocsNeededConstraint { return unknown; },
                     },
                     currBounds);
    };

    minBounds = applySkip(minBounds);
    maxBounds = applySkip(maxBounds);
}

void DocsNeededBoundsContext::applyLimit(long long limit) {
    // Apply the limit to max bounds, as long as it's less than the existing bounds.
    bool overridesMaxBounds = true;
    maxBounds =
        visit(OverloadedVisitor{
                  [&overridesMaxBounds, limit](long long discreteBounds) -> DocsNeededConstraint {
                      if (limit <= discreteBounds) {
                          return limit;
                      } else {
                          overridesMaxBounds = false;
                          return discreteBounds;
                      }
                  },
                  [limit](NeedAll) -> DocsNeededConstraint { return limit; },
                  [limit](Unknown) -> DocsNeededConstraint { return limit; },
              },
              maxBounds);

    // Apply the limit to min bounds, as long as it's less than the existing bounds.
    // In the specific edge case where the minBounds was Unknown but the maxBounds was a
    // value, we only want to set a minBounds here if the new limit also overrides
    // the maxBounds. Consider the pipeline [{$limit: 20}, {$densify}, {$limit: 10}] where, because
    // $densify could only increase the number of documents in the result stream, we should infer
    // that the maxBounds is 10 and the minBounds is unknown (i.e., we could need even fewer than
    // 10 documents). We need to avoid miscomputing minBounds as 20 after $densify sets minBounds
    // to Unknown.
    minBounds = visit(OverloadedVisitor{
                          [limit](long long discreteBounds) -> DocsNeededConstraint {
                              return std::min(discreteBounds, limit);
                          },
                          [limit](NeedAll) -> DocsNeededConstraint { return limit; },
                          [overridesMaxBounds, limit](Unknown unknown) -> DocsNeededConstraint {
                              // If minBounds is unknown, we don't want to set it if the limit
                              // didn't override the maxBounds. See comment above.
                              if (overridesMaxBounds) {
                                  return limit;
                              }
                              return unknown;
                          },
                      },
                      minBounds);
}

void DocsNeededBoundsContext::applyBlockingStage() {
    maxBounds = NeedAll();
    minBounds = NeedAll();
}
void DocsNeededBoundsContext::applyUnknownStage() {
    maxBounds = Unknown();
    minBounds = Unknown();
}

/**
 * For document sources without an explicitly-defined visitor, we'll throw out existing constraints
 * and set to Unknown.
 */
template <typename T>
void visit(DocsNeededBoundsContext* ctx, const T&) {
    LOGV2_DEBUG(
        8673201,
        5,
        "Encountered a DocumentSource with unimplemented visitor for DocsNeededBoundsContext.");
    ctx->applyUnknownStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceLimit& source) {
    ctx->applyLimit(source.getLimit());
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSkip& source) {
    ctx->applySkip(source.getSkip());
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSample& source) {
    // Although $sample caps the cardinality of the result stream like $limit, it internally
    // acts as a blocking stage.
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceGroupBase& source) {
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceGroup& source) {
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceStreamingGroup& source) {
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceBucketAuto& source) {
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceInternalSetWindowFields& source) {
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSort& source) {
    // Even if a $sort has absorbed a limit and is a top-k sort, it still needs to consume all
    // inputs.
    ctx->applyBlockingStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceMatch& source) {
    ctx->applyPossibleDecreaseStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceInternalShardFilter& source) {
    ctx->applyPossibleDecreaseStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceRedact& source) {
    // $redact may function like a $match if it redacts an entire document, so it may decrease
    // the number of documents in the result stream.
    ctx->applyPossibleDecreaseStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceUnwind& source) {
    // It's likely an $unwind stage would increase the cardinality of the result stream.
    // If preserveNullAndEmptyArrays is false, it's possible the stage could reduce cardinality as
    // well. The order of applying possible increase or decrease doesn't matter since they only
    // impact min or max bounds, respectively.
    ctx->applyPossibleIncreaseStage();
    if (!source.preserveNullAndEmptyArrays()) {
        ctx->applyPossibleDecreaseStage();
    }
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceUnionWith& source) {
    ctx->applyPossibleIncreaseStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceInternalDensify& source) {
    ctx->applyPossibleIncreaseStage();
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceTeeConsumer& source) {
    // DocumentSourceTeeConsumer is an internal proxy stage between a pipeline within a $facet stage
    // and the buffer of incoming documents. Because a DocumentSourceTeeConsumer stage is uniquely
    // positioned after each $facet subpipeline, we should do nothing. Otherwise, the visit will not
    // recognize the document source and override the $facet bounds by applying unknown bounds.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceFacet& source) {
    // Computes the DocsNeededBounds of each $facet subpipeline applied to the constraints of the
    // pipeline seen so far, and compares the constraints of each subpipeline to find the most
    // restrictive constraints of all the pipelines. After traversing all subpipelines, applies the
    // most restrictive constraints.

    auto& subpipelines = source.getFacetPipelines();
    if (subpipelines.empty()) {
        return;
    }

    // Initialize min/max bounds that are guaranteed to be overriden by the first subpipeline.
    // They're different values because of the different semantics of min vs max constraints for
    // determining constrait strength.
    DocsNeededConstraint mostRestrictiveMinBounds = Unknown();
    DocsNeededConstraint mostRestrictiveMaxBounds = 0;

    for (const auto& facetPipeline : subpipelines) {
        // Compute this subpipeline's constraints applied to the constraints-so-far.
        DocsNeededBoundsContext subpipelineCtx(ctx->minBounds, ctx->maxBounds);
        extractDocsNeededBoundsHelper(*facetPipeline.pipeline.get(), &subpipelineCtx);

        mostRestrictiveMinBounds = docs_needed_bounds::chooseStrongerMinConstraint(
            mostRestrictiveMinBounds, subpipelineCtx.minBounds);
        mostRestrictiveMaxBounds = docs_needed_bounds::chooseStrongerMaxConstraint(
            mostRestrictiveMaxBounds, subpipelineCtx.maxBounds);
    }

    // Apply the most restrictive bounds.
    ctx->minBounds = mostRestrictiveMinBounds;
    ctx->maxBounds = mostRestrictiveMaxBounds;
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSearch& source) {
    // No change, since $search is the stage that populates the result stream initially.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSearchMeta& source) {
    // No change, since $searchMeta is the stage that populates the result stream initially.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceInternalSearchMongotRemote& source) {
    // No change, since $_internalSearchMongotRemote is the stage that populates the result stream
    // initially.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceVectorSearch& source) {
    // No change, since $vectorStage is the stage that populates the result stream initially.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSingleDocumentTransformation& source) {
    // No change.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceFindAndModifyImageLookup& source) {
    // No change.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceGraphLookUp& source) {
    // No change, unless it's absorbed an $unwind.
    if (source.hasUnwindSource()) {
        visit(ctx, *source.getUnwindSource());
    }
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceLookUp& source) {
    // No change, unless it's absorbed an $unwind.
    if (source.hasUnwindSrc()) {
        visit(ctx, *source.getUnwindSource());
    }
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceMerge& source) {
    // No change.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceOut& source) {
    // No change.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSetVariableFromSubPipeline& source) {
    // No change.
}

void visit(DocsNeededBoundsContext* ctx, const DocumentSourceSequentialDocumentCache& source) {
    // No change. If the cache is in the "building" state, it acts as a no-op stage where it saves
    // each document to the cache (no change to result stream). If the cache is in the "abandoned"
    // state, it acts as a pure no-op. If the cache is in the "serving" state, this stage must be
    // the first in the pipeline, where it populates the result stream.
}

const ServiceContext::ConstructorActionRegisterer docsNeededBoundsRegisterer{
    "DocsNeededBoundsRegisterer", [](ServiceContext* service) {
        registerMongodVisitor<DocsNeededBoundsContext>(service);
    }};


DocsNeededBounds extractDocsNeededBounds(const Pipeline& pipeline) {
    DocsNeededBoundsContext ctx;
    extractDocsNeededBoundsHelper(pipeline, &ctx);
    return DocsNeededBounds(ctx.minBounds, ctx.maxBounds);
}
}  // namespace mongo
