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

#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include <algorithm>

namespace mongo::rule_based_rewrites::pipeline {

// Returns true if 'stage' is guaranteed not to alter the values of any field referenced by
// 'sortPattern'. A stage that passes preservesOrderAndMetadata but renames or computes a sort key
// field (e.g., $replaceRoot{newRoot:{a:"$b", b:"$a"}}) still invalidates the upstream sort
// guarantee for those fields, so we must stop the backward scan.
static bool sortKeyFieldsPreservedBy(const DocumentSource& stage, const SortPattern& sortPattern) {
    const auto* transform = dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(&stage);
    if (!transform) {
        // Only $_internalSearchIdLookup has been audited as a non-transforming pass-through that
        // cannot rename or compute sort key fields. Reject any other stage until it is explicitly
        // reviewed.
        return stage.isInstanceOf<DocumentSourceInternalSearchIdLookUp>();
    }
    if (transform->getTransformerType() != TransformerInterface::TransformerType::kReplaceRoot) {
        // TODO SERVER-127594: audit $project, $addFields/$set/$unset and extend field-mapping
        // inspection here before setting preservesOrderAndMetadata=true on those stages.
        return false;
    }
    const auto& replaceRoot =
        static_cast<const ReplaceRootTransformation&>(transform->getTransformer());
    const auto* exprObj = dynamic_cast<const ExpressionObject*>(replaceRoot.getExpression().get());
    if (!exprObj) {
        // The newRoot is not a plain object expression (e.g. $replaceWith:"$doc"). We can't inspect
        // individual field mappings, so the guarantee only holds when every sort part in the sort
        // pattern is metadata-based (no named field path) since metadata is unaffected by field
        // renaming.
        return std::all_of(sortPattern.begin(), sortPattern.end(), [](const auto& part) {
            return !part.fieldPath;
        });
    }
    const auto& children = exprObj->getChildExpressions();
    for (const auto& sortPart : sortPattern) {
        if (!sortPart.fieldPath) {
            continue;
        }
        const std::string fieldName{sortPart.fieldPath->fullPath()};
        auto it = std::find_if(
            children.begin(), children.end(), [&](const auto& c) { return c.first == fieldName; });
        if (it == children.end()) {
            return false;  // Field was dropped by the stage so the upstream sort order doesn't
                           // carry through.
        }
        const auto* fp = dynamic_cast<const ExpressionFieldPath*>(it->second.get());
        if (!fp || !fp->representsPath(fieldName)) {
            return false;  // Field is renamed or computed, therefore sort pattern is not preserved.
        }
    }
    return true;
}

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
        // Even if the stage preserves document order, it may rename or compute sort key fields
        // (e.g., $replaceRoot{newRoot:{a:"$b",b:"$a"}}), invalidating the upstream guarantee.
        if (!sortKeyFieldsPreservedBy(*stage, sortPattern)) {
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
