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

#include "mongo/db/pipeline/optimization/sort_rules.h"

#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo::rule_based_rewrites::pipeline {

bool sortIsRedundantGivenPrecedingStages(const PipelineRewriteContext& ctx) {
    if (ctx.atFirstStage()) {
        return false;
    }
    const auto& sort = ctx.currentAs<DocumentSourceSort>();

    // getLimit(): an absorbed $limit would be silently erased too, changing result cardinality.
    // isBoundedSortStage(): time-series _timeSorter has execution semantics beyond reordering.
    if (sort.getLimit() || sort.isBoundedSortStage()) {
        return false;
    }
    const bool needsSortKeyMetadata = sort.providesSortKeyMetadata();
    const SortPattern& sortPattern = sort.getSortPattern();
    // A stage that preserves order in kUnsplit may not do so on the merge side (kSplitForMerge),
    // so pass the actual split state when querying constraints.
    const PipelineSplitState splitState = sort.getExpCtx()->getNeedsMerge()
        ? PipelineSplitState::kSplitForMerge
        : PipelineSplitState::kUnsplit;
    for (const auto& stage : ctx.prevStagesRange()) {
        const SortPattern& stagePattern = stage->getSortPattern();
        if (!stagePattern.empty()) {
            tassert(12397200,
                    "A stage that establishes its own sort pattern must not also declare "
                    "preservesOrderAndMetadata=true; these properties are logically contradictory",
                    !stage->constraints(splitState).preservesOrderAndMetadata);
            if (!stagePattern.isExtensionOf(sortPattern)) {
                return false;
            }
            // Safe to remove even if this $sort sets $sortKey metadata, provided the preceding
            // sort-establishing stage already provides it (e.g. $vectorSearch, $search, or a
            // $sort whose providesSortKeyMetadata() returns true).
            return !needsSortKeyMetadata || stage->providesSortKeyMetadata();
        }
        // A stage that does not preserve order may reorder or reshuffle documents, so any sort
        // guarantee established before it does not carry through to the current $sort.
        if (!stage->constraints(splitState).preservesOrderAndMetadata) {
            return false;
        }
    }
    return false;
}

namespace {
REGISTER_RULES(DocumentSourceSort,
               {
                   .name = "REDUNDANT_SORT_REMOVAL",
                   .precondition = sortIsRedundantGivenPrecedingStages,
                   .transform = Transforms::eraseCurrent,
                   .priority = kDefaultOptimizeAtPriority + 1,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               });
}  // namespace

}  // namespace mongo::rule_based_rewrites::pipeline
