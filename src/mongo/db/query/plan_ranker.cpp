/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_ranker.h"

#include "mongo/logv2/log.h"

namespace mongo::plan_ranker {
namespace log_detail {
void logScoreFormula(std::function<std::string()> formula,
                     double score,
                     double baseScore,
                     double productivity,
                     double noFetchBonus,
                     double noSortBonus,
                     double noIxisectBonus,
                     double tieBreakers) {
    LOGV2_DEBUG(20961, 2, "Score formula", "formula"_attr = [&]() {
        StringBuilder sb;
        sb << "score(" << str::convertDoubleToString(score) << ") = baseScore("
           << str::convertDoubleToString(baseScore) << ")"
           << " + productivity(" << formula() << " = " << str::convertDoubleToString(productivity)
           << ")"
           << " + tieBreakers(" << str::convertDoubleToString(noFetchBonus) << " noFetchBonus + "
           << str::convertDoubleToString(noSortBonus) << " noSortBonus + "
           << str::convertDoubleToString(noIxisectBonus)
           << " noIxisectBonus = " << str::convertDoubleToString(tieBreakers) << ")";
        return sb.str();
    }());
}

void logScoreBoost(double score) {
    LOGV2_DEBUG(20962, 5, "Score boosted due to intersection forcing", "newScore"_attr = score);
}

void logScoringPlan(std::function<std::string()> solution,
                    std::function<std::string()> explain,
                    std::function<std::string()> planSummary,
                    size_t planIndex,
                    bool isEOF) {
    LOGV2_DEBUG(20956,
                5,
                "Scoring plan",
                "planIndex"_attr = planIndex,
                "querySolution"_attr = redact(solution()),
                "stats"_attr = redact(explain()));
    LOGV2_DEBUG(20957,
                2,
                "Scoring query plan",
                "planSummary"_attr = planSummary(),
                "planHitEOF"_attr = isEOF);
}

void logScore(double score) {
    LOGV2_DEBUG(20958, 5, "Basic plan score", "score"_attr = score);
}

void logEOFBonus(double eofBonus) {
    LOGV2_DEBUG(20959, 5, "Adding EOF bonus to score", "eofBonus"_attr = eofBonus);
}

void logFailedPlan(std::function<std::string()> planSummary) {
    LOGV2_DEBUG(
        20960, 2, "Not scoring a plan because the plan failed", "planSummary"_attr = planSummary());
}
}  // namespace log_detail

namespace {
/**
 * A plan scorer for the classic plan stage tree. Defines the plan productivity as a number
 * of intermediate results returned, or advanced, by the root stage, divided by the "unit of works"
 * which the plan performed. Each call to work(...) counts as one unit.
 */
class DefaultPlanScorer final : public PlanScorer<PlanStageStats> {
protected:
    double calculateProductivity(const PlanStageStats* stats) const final {
        invariant(stats->common.works != 0);
        return static_cast<double>(stats->common.advanced) /
            static_cast<double>(stats->common.works);
    }

    std::string getProductivityFormula(const PlanStageStats* stats) const {
        StringBuilder sb;
        sb << "(" << stats->common.advanced << " advanced)/(" << stats->common.works << " works)";
        return sb.str();
    }

    double getNumberOfAdvances(const PlanStageStats* stats) const final {
        return stats->common.works;
    }

    bool hasStage(StageType type, const PlanStageStats* root) const final {
        std::queue<const PlanStageStats*> remaining;
        remaining.push(root);

        while (!remaining.empty()) {
            auto stats = remaining.front();
            remaining.pop();

            if (stats->stageType == type) {
                return true;
            }

            for (auto&& child : stats->children) {
                remaining.push(child.get());
            }
        }
        return false;
    }
};
}  // namespace

std::unique_ptr<PlanScorer<PlanStageStats>> makePlanScorer() {
    return std::make_unique<DefaultPlanScorer>();
}
}  // namespace mongo::plan_ranker
