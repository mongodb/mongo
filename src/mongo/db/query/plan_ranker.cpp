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


#include "mongo/db/query/plan_ranker.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <queue>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::plan_ranker {

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

    std::string getProductivityFormula(const PlanStageStats* stats) const override {
        StringBuilder sb;
        sb << "(" << stats->common.advanced << " advanced)/(" << stats->common.works << " works)";
        return sb.str();
    }

    double getNumberOfAdvances(const PlanStageStats* stats) const final {
        return stats->common.advanced;
    }

    bool hasStage(StageType type, const PlanStageStats* root) const final {
        return hasStage(root,
                        [type](const PlanStageStats& stage) { return stage.stageType == type; });
    }

    bool hasFetch(const PlanStageStats* root) const final {
        return hasStage(root, [](const PlanStageStats& stage) {
            return stage.specific && stage.specific->doesFetch();
        });
    }

    bool hasStage(const PlanStageStats* root,
                  std::function<bool(const PlanStageStats&)> filter) const {
        std::queue<const PlanStageStats*> remaining;
        remaining.push(root);

        while (!remaining.empty()) {
            auto stats = remaining.front();
            remaining.pop();

            if (filter(*stats)) {
                return true;
            }

            for (auto&& child : stats->children) {
                remaining.push(child.get());
            }
        }
        return false;
    }
};

/**
 * Return true if the nodes have the same type and the same number of children.
 */
bool areNodesCompatible(const std::vector<const QuerySolutionNode*>& nodes) {
    for (size_t i = 1; i < nodes.size(); ++i) {
        if (nodes[i - 1]->getType() != nodes[i]->getType()) {
            return false;
        }

        if (nodes[i - 1]->children.size() != nodes[i]->children.size()) {
            return false;
        }
    }

    return true;
}

/**
 * The function tries to detect if the interval is closed on both ends. Can return false
 * positive results for the types not mentioned in the comment to 'isMinMaxValue' function.
 */
bool isClosedInterval(const Interval& interval) {
    // If the bound types are different the interval is considered to be open.
    if (interval.start.type() != interval.end.type()) {
        return false;
    }

    switch (interval.getDirection()) {
        // Point intervals, empty intervals, and null intervals have no direction.
        case Interval::Direction::kDirectionNone:
            return true;
        case Interval::Direction::kDirectionAscending:
            return !isLowerBound(interval.start, interval.startInclusive) &&
                !isUpperBound(interval.end, interval.endInclusive);
        case Interval::Direction::kDirectionDescending:
            return !isUpperBound(interval.start, interval.startInclusive) &&
                !isLowerBound(interval.end, interval.endInclusive);
    }

    MONGO_UNREACHABLE_TASSERT(8102102);
}

/**
 * Returns true if this OIL contains only closed intervals.
 */
bool containsOnlyClosedIntervals(const OrderedIntervalList& oil) {
    for (const auto& interval : oil.intervals) {
        if (!isClosedInterval(interval)) {
            return false;
        }
    }

    return true;
}

/**
 * Calculates score for the given index bounds. The score reflects the following rules:
 * - IndexBounds that has longest single point interval prefix wins,
 * - if winner is not defined on the previous step then IndexBounds with the longest point
 * interval prefix wins,
 * - if winner is not defined on the previous step then IndexBounds with the longest closed
 * interval prefix wins,
 * - if winner is not defined, then IndexBounds with longest interval prefix wins
 * - if winner is not defined, them IndexBounds with shortest index key pattern wins.
 */
uint64_t getIndexBoundsScore(const IndexBounds& bounds) {
    const uint64_t indexKeyLength = static_cast<uint64_t>(bounds.fields.size());
    uint64_t singlePointIntervalPrefix = 0;
    uint64_t pointsIntervalPrefix = 0;
    uint64_t closedIntervalPrefix = 0;
    uint64_t intervalLength = 0;

    for (const auto& field : bounds.fields) {
        // Skip the $** index virtual field, as it's not part of the actual index key.
        if (field.name == "$_path") {
            continue;
        }

        // Stop scoring index bounds as soon as we see an all-values interval.
        if (field.isMinToMax() || field.isMaxToMin()) {
            break;
        }

        if (intervalLength == singlePointIntervalPrefix && field.isPoint()) {
            ++singlePointIntervalPrefix;
        }

        if (intervalLength == pointsIntervalPrefix && field.containsOnlyPointIntervals()) {
            ++pointsIntervalPrefix;
        }

        if (intervalLength == closedIntervalPrefix && containsOnlyClosedIntervals(field)) {
            ++closedIntervalPrefix;
        }

        ++intervalLength;
    }

    // We pack calculated stats into one value to make their comparison simplier. For every
    // prefix length we allocate 12 bits (4096 values) which is more then enough since an index
    // can have no more than 32 fields (see "MongoDB Limits and Thresholds" reference).
    // 'indexKeyLength' is treated differently because, unlike others, we prefer shorter index
    // key prefix length (see the comment to the function for details).
    uint64_t result = (singlePointIntervalPrefix << 52) | (pointsIntervalPrefix << 40) |
        (closedIntervalPrefix << 28) | (intervalLength << 16) |
        (std::numeric_limits<uint16_t>::max() - indexKeyLength);

    return result;
}

/**
 * Calculates scores for the given IndexBounds and add 1 to every winner's resultScores. i-th
 * position in resultScores corresponds to i-th field in IndexBound.
 */
void scoreIndexBounds(const std::vector<const IndexBounds*>& bounds,
                      std::vector<size_t>& resultScores) {
    const size_t nfields = bounds.size();

    std::vector<uint64_t> scores{};
    scores.reserve(nfields);
    for (size_t i = 0; i < bounds.size(); ++i) {
        scores.emplace_back(getIndexBoundsScore(*bounds[i]));
    }

    auto topScore = max_element(scores.begin(), scores.end());
    for (size_t i = 0; i < nfields; ++i) {
        if (*topScore == scores[i]) {
            resultScores[i] += 1;
        }
    }
}
}  // namespace

std::unique_ptr<PlanScorer<PlanStageStats>> makePlanScorer() {
    return std::make_unique<DefaultPlanScorer>();
}

std::vector<size_t> applyIndexPrefixHeuristic(const std::vector<const QuerySolution*>& solutions) {
    std::vector<size_t> solutionScores(solutions.size(), 0);

    std::vector<std::vector<const QuerySolutionNode*>> stack{};
    stack.emplace_back();
    stack.back().reserve(solutions.size());
    for (auto solution : solutions) {
        stack.back().emplace_back(solution->root());
    }

    while (!stack.empty()) {
        auto top = std::move(stack.back());
        stack.pop_back();

        if (!areNodesCompatible(top)) {
            return {};
        }

        // Compatible nodes have the same number of children, see comment to 'areNodesCompatible'
        // function.
        for (size_t childIndex = 0; childIndex < top.front()->children.size(); ++childIndex) {
            stack.emplace_back();
            stack.back().reserve(solutions.size());
            for (auto node : top) {
                stack.back().emplace_back(node->children[childIndex].get());
            }
        }

        if (top.front()->getType() == STAGE_IXSCAN ||
            top.front()->getType() == STAGE_DISTINCT_SCAN) {
            std::vector<const IndexBounds*> bounds{};
            bounds.reserve(solutions.size());

            for (auto&& node : top) {
                const IndexBounds* nodeBounds;
                switch (node->getType()) {
                    case STAGE_IXSCAN: {
                        nodeBounds = &static_cast<const IndexScanNode*>(node)->bounds;
                        break;
                    }
                    case STAGE_DISTINCT_SCAN: {
                        nodeBounds = &static_cast<const DistinctNode*>(node)->bounds;
                        break;
                    }
                    default: {
                        tasserted(9844200,
                                  str::stream()
                                      << "Unexpected node type for index prefix heuristic "
                                      << node->getType());
                    }
                }
                bounds.emplace_back(nodeBounds);
            }

            scoreIndexBounds(bounds, solutionScores);
        }
    }

    std::vector<size_t> winningSolutionIndices{};
    winningSolutionIndices.reserve(solutions.size());
    const auto topScore = max_element(solutionScores.begin(), solutionScores.end());
    for (size_t index = 0; index < solutionScores.size(); ++index) {
        if (solutionScores[index] == *topScore) {
            winningSolutionIndices.emplace_back(index);
        }
    }

    return winningSolutionIndices;
}
}  // namespace mongo::plan_ranker
