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

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"

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
struct DocsNeededBoundsContext : public DocumentSourceVisitorContextBase {
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
DocsNeededBounds extractDocsNeededBounds(const Pipeline& pipeline);
}  // namespace mongo
