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

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_ranker.h"

namespace mongo {
/**
 * A PlanExplainer implementation for SBE execution plans.
 */
class PlanExplainerSBE final : public PlanExplainer {
public:
    PlanExplainerSBE(const sbe::PlanStage* root,
                     const stage_builder::PlanStageData* data,
                     const QuerySolution* solution,
                     std::vector<sbe::plan_ranker::CandidatePlan> rejectedCandidates,
                     bool isMultiPlan)
        : PlanExplainer{solution},
          _root{root},
          _rootData{data},
          _solution{solution},
          _rejectedCandidates{std::move(rejectedCandidates)},
          _isMultiPlan{isMultiPlan} {}

    bool isMultiPlan() const final {
        return _isMultiPlan;
    }

    const ExplainVersion& getVersion() const final;
    std::string getPlanSummary() const final;
    void getSummaryStats(PlanSummaryStats* statsOut) const final;
    PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const final;
    std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const final;
    std::vector<PlanStatsDetails> getCachedPlanStats(const PlanCacheEntry::DebugInfo&,
                                                     ExplainOptions::Verbosity) const final;

private:
    boost::optional<BSONObj> buildExecPlanDebugInfo(
        const sbe::PlanStage* root, const stage_builder::PlanStageData* data) const {
        if (root && data) {
            return BSON("slots" << data->debugString() << "stages"
                                << sbe::DebugPrinter().print(*_root));
        }
        return boost::none;
    }

    // These fields are are owned elsewhere (e.g. the PlanExecutor or CandidatePlan).
    const sbe::PlanStage* _root{nullptr};
    const stage_builder::PlanStageData* _rootData{nullptr};
    const QuerySolution* _solution{nullptr};

    const std::vector<sbe::plan_ranker::CandidatePlan> _rejectedCandidates;
    const bool _isMultiPlan{false};
};
}  // namespace mongo
