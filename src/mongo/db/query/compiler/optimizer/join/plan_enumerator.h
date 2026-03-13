/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Describes shape of plan tree.
 */
enum class PlanTreeShape { LEFT_DEEP, RIGHT_DEEP, ZIG_ZAG };

/**
 * Determines what plans we enumerate.
 */
enum class PlanEnumerationMode {
    // Only enumerate plans if they are cheaper than the lowest-cost plan for each subset.
    CHEAPEST,
    // Enumerates all plans, regardless of cost.
    ALL,
    // Enumerates a plan based on hints.
    HINTED
};

/**
 * Hints how a join should be done at the current subset level.
 */
struct JoinHint {
    // The next node to join with.
    NodeId node;
    // The next join method to use. Ignored for base subset.
    JoinMethod method;
    /**
     * Indicates if the base collection access node is the left or right child of this node.
     * Ignored for base subset. For example, this hint vector (let every entry indicate one subset
     * level):
     *   {{INLJ, 1, true}, {HJ, 2, true}, {NLJ, 3, false}}
     * would result in the following tree:
     *
     *          NLJ
     *         /   \
     *        HJ    3
     *       /  \
     *      2    1
     *
     * We ignore the join method/isLeftChild field for the first level (node 1). We place node 2 on
     * the left side of a HJ ('isLeftChild' = true), and our current subtree on the right. Finally,
     * we place an NLJ with node 3 on the right ('isLeftChild' = false).
     */
    bool isLeftChild;

    BSONObj toBSON() const;
};

/**
 * This structure allows us to specify a particular enumeration mode per subset level. Note that:
 *  - A mode must always be specified for level 0.
 *  - It is not permitted to specify the same exact mode for two consecutive entries.
 *
 * The default mode is:
 *  {{0, CHEAPEST}}
 *
 * This means that for all subset levels (including 0), we will use the "CHEAPEST" enumeration mode.
 *
 * Modes are "sticky" until a the next entry specifying a new mode for a level is found, i.e. levels
 * keep using the mode last specified for the previous level unless there is an entry specifically
 * for that level. For example:
 *  {{0, CHEAPEST}, {2, ALL}, {4, CHEAPEST}}
 *
 * For subset levels 0 & 1, we will apply the "CHEAPEST" enumeration mode. Then, for subsets 2 & 3,
 * we will apply all plans enumeration (ALL). Finally, for any subset level 4+, we go back to
 * picking the cheapest subset.
 */
class PerSubsetLevelEnumerationMode {
public:
    /**
     * Describes enumeration strategy for a given subset level and above.
     */
    struct SubsetLevelMode {
        // First level at which to apply this mode.
        size_t level;
        PlanEnumerationMode mode;
        // Optionally configures plan enumeration via hints.
        boost::optional<JoinHint> hint = boost::none;

        BSONObj toBSON() const;
    };

    PerSubsetLevelEnumerationMode(PlanEnumerationMode mode);
    PerSubsetLevelEnumerationMode(std::vector<SubsetLevelMode> modes);

    struct Iterator {
        Iterator& next() {
            if (_index < _mode._modes.size()) {
                _index++;
            }
            return *this;
        }

        bool operator==(const Iterator& other) const {
            tassert(
                11391603, "Must be comparing iterators on same instance", &_mode == &other._mode);
            return _index == other._index;
        }

        auto get() const {
            tassert(11391604, "Must not be end iterator", _index < _mode._modes.size());
            return _mode._modes[_index];
        }

    private:
        Iterator(const PerSubsetLevelEnumerationMode& mode, size_t index)
            : _mode{mode}, _index{index} {}

        const PerSubsetLevelEnumerationMode& _mode;
        size_t _index;
        friend PerSubsetLevelEnumerationMode;
    };

    Iterator begin() const {
        return Iterator(*this, 0);
    };

    Iterator end() const {
        return Iterator(*this, _modes.size());
    };

    BSONObj toBSON() const;

private:
    const std::vector<SubsetLevelMode> _modes;
    friend PerSubsetLevelEnumerationMode::Iterator;
};

/**
 * This configures the kinds of plans we're generating and how we're choosing between them during
 * enumeration.
 */
struct EnumerationStrategy {
    PlanTreeShape planShape;
    PerSubsetLevelEnumerationMode mode;
    bool enableHJOrderPruning;
};

/**
 * Context containing all the state for the bottom-up dynamic programming join plan enumeration
 * algorithm.
 */
class PlanEnumeratorContext {
public:
    PlanEnumeratorContext(const JoinReorderingContext& ctx,
                          JoinCardinalityEstimator* estimator,
                          JoinCostEstimator* coster,
                          EnumerationStrategy strategy)
        : _ctx{ctx},
          _estimator(std::move(estimator)),
          _coster(std::move(coster)),
          _strategy(std::move(strategy)) {}

    // Delete copy and move operations to ensure concrete ownership.
    PlanEnumeratorContext(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext& operator=(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext(PlanEnumeratorContext&&) = delete;
    PlanEnumeratorContext& operator=(PlanEnumeratorContext&&) = delete;

    /**
     * Return all 'JoinSubsets' of size level+1.
     */
    const std::vector<JoinSubset>& getSubsets(int level);

    /**
     * Enumerates all join subsets in bottom-up fashion.
     */
    void enumerateJoinSubsets();

    /**
     * Did the plan enumerator find a plan?
     */
    bool enumerationSuccessful() const {
        return _joinSubsets[_joinSubsets.size() - 1].size() == 1 &&
            !_joinSubsets[_joinSubsets.size() - 1][0].plans.empty();
    }

    JoinPlanNodeId getBestFinalPlan() const {
        return finalSubset().bestPlan();
    }

    /**
     * Returns all plan node ids for plans enumerated in the last subset other than the best plan.
     * Used for explain().
     */
    std::vector<JoinPlanNodeId> getRejectedFinalPlans() const;

    const JoinPlanNodeRegistry& registry() const {
        return _registry;
    }

    JoinCardinalityEstimator* getJoinCardinalityEstimator() const {
        return _estimator;
    }

    JoinCostEstimator* getJoinCostEstimator() const {
        return _coster;
    }

    /**
     * Used for testing & debugging.
     */
    std::string toString() const;
    const EnumerationStrategy& getStrategy() const {
        return _strategy;
    }

private:
    const JoinSubset& finalSubset() const {
        tassert(
            11336904, "Expected subsets to have already been enumerated", enumerationSuccessful());
        return _joinSubsets[_joinSubsets.size() - 1][0];
    }

    /**
     * Enumerate plans by constructing possible joins between the 'left' and 'right' subsets and
     * outputting those plans in 'cur'. Note that 'left' and 'right' must be disjoint, and their
     * union must produce 'cur'.
     */
    void enumerateJoinPlans(const JoinSubset& left, const JoinSubset& right, JoinSubset& cur);

    /**
     * Helpers for adding a join plan to subset 'cur', constructed using the specified join 'method'
     * connecting the best plans from the provided subsets in 'CHEAPEST' enuemration mode, and every
     * pair of plans in 'ALL' plans enumeration mode.
     */
    void addJoinPlan(JoinMethod method,
                     const JoinSubset& left,
                     const JoinSubset& right,
                     const std::vector<EdgeId>& edges,
                     JoinSubset& cur);
    void enumerateAllJoinPlans(JoinMethod method,
                               const JoinSubset& left,
                               const JoinSubset& right,
                               const std::vector<EdgeId>& edges,
                               JoinSubset& subset);
    void enumerateCheapestJoinPlan(JoinMethod method,
                                   const JoinSubset& left,
                                   const JoinSubset& right,
                                   const std::vector<EdgeId>& edges,
                                   JoinSubset& subset);
    void enumerateHintedPlan(JoinMethod method,
                             const JoinSubset& left,
                             const JoinSubset& right,
                             const std::vector<EdgeId>& edges,
                             JoinSubset& subset);

    /**
     * Helper for enumerating an INLJ plan with the specified left subtree & generating an index
     * probe on the RHS for the subset 'right'.
     */
    void enumerateINLJPlan(EdgeId edge,
                           JoinPlanNodeId leftPlan,
                           const JoinSubset& right,
                           JoinSubset& subset);

    /**
     * Helper to enumerate an NLJ/HJ join plan combining the subtrees identified by 'leftPlan' &
     * 'rightPlan'.
     */
    void enumerateJoinPlan(JoinMethod method,
                           JoinPlanNodeId leftPlan,
                           JoinPlanNodeId rightPlan,
                           JoinSubset& subset);

    /**
     * Determines based on the shape of the tree obtained by joining the best plans on each side if
     * we would retain the tree shape specified by 'type' and the plan is valid for the given join
     * 'method'.
     */
    bool canPlanBeEnumerated(JoinMethod method,
                             const JoinSubset& left,
                             const JoinSubset& right,
                             const JoinSubset& subset);

    void addPlanToSubset(JoinMethod method,
                         JoinPlanNodeId left,
                         JoinPlanNodeId right,
                         JoinCostEstimate cost,
                         JoinSubset& subset,
                         bool isBestPlan);

    inline bool isBestPlanSoFar(const JoinSubset& subset, const JoinCostEstimate& planCost) const {
        return !subset.hasPlans() || (planCost < _registry.getCost(subset.bestPlan()));
    }

    const JoinReorderingContext& _ctx;
    // Unowned pointers! If null, all costs are set to 0.
    JoinCardinalityEstimator* _estimator;
    JoinCostEstimator* _coster;
    EnumerationStrategy _strategy;

    // Variable tracking current enumeration mode during enumeration.
    PerSubsetLevelEnumerationMode::SubsetLevelMode _mode{.level = 0,
                                                         .mode = PlanEnumerationMode::CHEAPEST};

    // Hold intermediate results of the enumeration algorithm. The index into the outer vector
    // represents the "level". The i'th level contains solutions for the optimal way to join all
    // possible subsets of size i+1.
    std::vector<std::vector<JoinSubset>> _joinSubsets;

    // Memory management for trees so we can reuse nodes.
    JoinPlanNodeRegistry _registry;
};

}  // namespace mongo::join_ordering
