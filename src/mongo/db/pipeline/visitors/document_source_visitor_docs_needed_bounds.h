// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/util/modules.h"

namespace mongo {
using DocsNeededConstraint = docs_needed_bounds::DocsNeededConstraint;

/**
 * The visitor context used to compute the number of documents needed for a pipeline, assuming
 * the pipeline will be traversed in reverse order. Tracks the minimum and maximum constraints
 * inferred so far.
 *
 * NOTE: Any blocking stages (like $sort, $group) should use only applyBlockingStage(), since the
 * result cardinality of a stage doesn't matter if it requires all documents as input.
 */
struct [[MONGO_MOD_PUBLIC]] DocsNeededBoundsContext : public DocumentSourceVisitorContextBase {
    DocsNeededBoundsContext(DocsNeededConstraint startingMinBounds = docs_needed_bounds::Unknown(),
                            DocsNeededConstraint startingMaxBounds = docs_needed_bounds::Unknown())
        : minBounds(startingMinBounds), maxBounds(startingMaxBounds) {}

    void applyPossibleDecreaseStage();
    void applyPossibleIncreaseStage();
    void applySkip(long long newSkip);
    void applyLimit(long long newLimit);
    void applyBlockingStage();
    void applyUnknownStage();

    DocsNeededConstraint minBounds;
    DocsNeededConstraint maxBounds;
};

/**
 * Walks the pipeline to infer lower and upper bound constraints on how many documents
 * will need to be scanned / retrieved to satisfy the query. Returns the DocsNeededBounds,
 * (with the minimum lower bound constraint and the maximum upper bound constraint), each of type
 * long long, NeedAll, or Unknown.
 *
 * We walk the pipeline back-to-front to observe how each stage in the pipeline will affect the
 * minimum and maximum bounds on documents needed in the pipeline. For example, reaching a
 * blocking stage means we need all documents (NeedAll), and reaching a $limit stage means we can
 * constrict the min/max bounds to the discrete limit value.
 *
 * As we iterate to visit each stage in reverse order, we check the existing bounds to consider
 * overriding them. For example, with the pipeline [{$limit}, {$sort}], the limit value will
 * override the NeedAll imposed by $sort, since we only need to sort the limit-ed documents. But,
 * with the pipeline [{$sort}, {$limit}], the $sort supercedes the $limit's value with
 * NeedAll since the limit is applied to the sorted set of all documents.
 *
 * Not all stages override existing bounds. In the pipeline [{$match}, {$sort}], the NeedAll
 * from the $sort stage persists through the $match (we still need all documents, even if some are
 * discarded by $match). For othe pipeline [{$match}, {$limit}], the minimum bounds imposed by
 * the $limit persists through a $match (since $match could discard 0 documents), but the maximum
 * bounds becomes Unknown (since $match could discard many documents).
 *
 * WARNING: This function is currently used only for $search, to determine how many documents to
 * request from mongot. For that reason, this function may not properly handle all stages that are
 * incompatible with $search, like any stage that has PositionRequirement::kFirst, or anything
 * $changeStream-related. For now, those stages will cause the function to override any inferred
 * constraints with Unknown.
 * If you're looking to expand the usage of this function, you will need to
 * define more stage-specific visitors in this file or other
 * document_source_visitor_docs_needed_bounds.cpp files in other modules.
 */
void visitExtensionStage(DocsNeededBoundsContext* ctx,
                         const extension::host::DocumentSourceExtensionOptimizable& source);
void visitExtensionStage(DocsNeededBoundsContext* ctx,
                         const extension::host::DocumentSourceExtensionForQueryShape& source);

DocsNeededBounds extractDocsNeededBounds(const DocumentSourceContainer& sources,
                                         const ExpressionContext& expCtx);
DocsNeededBounds extractDocsNeededBounds(const ConstDocumentSourceContainer& sources,
                                         const ExpressionContext& expCtx);

DocsNeededBounds extractDocsNeededBounds(const Pipeline& pipeline);
}  // namespace mongo
