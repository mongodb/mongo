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

#include "mongo/db/pipeline/plan_explainer_pipeline.h"

#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/util/assert_util.h"

#include <algorithm>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
const PlanExplainer::ExplainVersion& PlanExplainerPipeline::getVersion() const {
    static const ExplainVersion kExplainVersion = "1";

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().front().get())) {
        return docSourceCursor->getExplainVersion();
    }
    return kExplainVersion;
}

std::string PlanExplainerPipeline::getPlanSummary() const {
    if (auto cursorStage =
            dynamic_cast<exec::agg::CursorStage*>(_execPipeline->getStages().front().get())) {
        return cursorStage->getPlanSummaryStr();
    }

    return "";
}

void PlanExplainerPipeline::getSummaryStats(PlanSummaryStats* statsOut) const {
    tassert(9378603, "Encountered unexpected nullptr for PlanSummaryStats", statsOut);

    auto stage_it = _execPipeline->getStages().cbegin();
    if (auto cursorStage = dynamic_cast<exec::agg::CursorStage*>(stage_it->get())) {
        *statsOut = cursorStage->getPlanSummaryStats();
        ++stage_it;
    };

    PlanSummaryStatsVisitor visitor(*statsOut);
    std::for_each(stage_it, _execPipeline->getStages().cend(), [&](const auto& stage) {
        statsOut->usedDisk = statsOut->usedDisk || stage->usedDisk();
        if (auto specificStats = stage->getSpecificStats()) {
            specificStats->acceptVisitor(&visitor);
        }
    });

    statsOut->nReturned = _nReturned;
}

PlanExplainer::PlanStatsDetails PlanExplainerPipeline::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    // TODO SERVER-49808: Report execution stats for the pipeline.
    if (_pipeline->empty()) {
        return {};
    }

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().cbegin()->get())) {
        if (auto explainer = docSourceCursor->getPlanExplainer()) {
            return explainer->getWinningPlanStats(verbosity);
        }
    };
    return {};
}

PlanExplainer::PlanStatsDetails PlanExplainerPipeline::getWinningPlanTrialStats() const {
    // We are not supposed to call this method on a pipeline explainer.
    MONGO_UNREACHABLE;
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerPipeline::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    // Multi-planning is not supported for aggregation pipelines.
    return {};
}
}  // namespace mongo
