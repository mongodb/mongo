/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/plan_explainer_pipeline.h"

#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/explain.h"

namespace mongo {
/**
 * Templatized method to get plan summary stats from document source and aggregate it to 'statsOut'.
 */
template <typename DocSourceType, typename DocSourceStatType>
void collectPlanSummaryStats(const DocSourceType& source, PlanSummaryStats* statsOut) {
    auto specificStats = source.getSpecificStats();
    invariant(specificStats);
    auto& docSpecificStats = static_cast<const DocSourceStatType&>(*specificStats);
    statsOut->accumulate(docSpecificStats.planSummaryStats);
}

std::string PlanExplainerPipeline::getPlanSummary() const {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().front().get())) {
        return docSourceCursor->getPlanSummaryStr();
    }

    return "";
}

void PlanExplainerPipeline::getSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().front().get())) {
        *statsOut = docSourceCursor->getPlanSummaryStats();
    }

    for (auto&& source : _pipeline->getSources()) {
        statsOut->usedDisk = statsOut->usedDisk || source->usedDisk();

        if (dynamic_cast<DocumentSourceSort*>(source.get())) {
            statsOut->hasSortStage = true;
        } else if (auto docSourceLookUp = dynamic_cast<DocumentSourceLookUp*>(source.get())) {
            collectPlanSummaryStats<DocumentSourceLookUp, DocumentSourceLookupStats>(
                *docSourceLookUp, statsOut);
        } else if (auto docSourceUnionWith = dynamic_cast<DocumentSourceUnionWith*>(source.get())) {
            collectPlanSummaryStats<DocumentSourceUnionWith, UnionWithStats>(*docSourceUnionWith,
                                                                             statsOut);
        }
    }

    if (_nReturned) {
        statsOut->nReturned = _nReturned;
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerPipeline::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    // TODO SERVER-49808: Report execution stats for the pipeline.
    return {};
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerPipeline::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    // Multi-planning is not supported for aggregation pipelines.
    return {};
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerPipeline::getCachedPlanStats(
    const PlanCacheEntry::DebugInfo&, ExplainOptions::Verbosity) const {
    // Pipelines are not cached, so we should never try to rebuild the stats from a cached entry.
    MONGO_UNREACHABLE;
}
}  // namespace mongo
