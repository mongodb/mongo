// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include <memory>

namespace mongo::parallelism_planner {

inline constexpr int kMaxDoPForTests = 8;

/**
 * A single-child node that reports a sort order, for exercising sort-preserving gathers.
 */
struct MockSortedNode : public QuerySolutionNode {
    MockSortedNode(std::unique_ptr<QuerySolutionNode> child, BSONObj pattern)
        : QuerySolutionNode(std::move(child)), sorts(std::move(pattern)) {}

    bool isParallelEligible() const override {
        return true;
    }

    StageType getType() const override {
        return STAGE_SORT_DEFAULT;
    }
    void appendToString(str::stream* ss, int indent) const override {
        *ss << "MOCK_SORT\n";
    }
    bool fetched() const override {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& f) const override {
        return children[0]->getFieldAvailability(f);
    }
    bool sortedByDiskLoc() const override {
        return false;
    }
    const ProvidedSortSet& providedSorts() const override {
        return sorts;
    }
    std::unique_ptr<QuerySolutionNode> clone() const override {
        return std::make_unique<MockSortedNode>(children[0]->clone(), sorts.getBaseSortPattern());
    }

    ProvidedSortSet sorts;
};

struct IneligibleNode : public QuerySolutionNode {
    explicit IneligibleNode(std::unique_ptr<QuerySolutionNode> child)
        : QuerySolutionNode(std::move(child)) {}

    bool isParallelEligible() const override {
        return false;
    }

    StageType getType() const override {
        return STAGE_FETCH;
    }
    void appendToString(str::stream* ss, int indent) const override {
        *ss << "INELIGIBLE\n";
    }
    bool fetched() const override {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string&) const override {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const override {
        return false;
    }
    const ProvidedSortSet& providedSorts() const override {
        return kEmptySet;
    }
    std::unique_ptr<QuerySolutionNode> clone() const override {
        return std::make_unique<IneligibleNode>(children[0]->clone());
    }
};

// Plan-building helpers shared by various parallelism planner tests.
inline std::unique_ptr<QuerySolutionNode> scan() {
    return std::make_unique<CollectionScanNode>();
}

inline std::unique_ptr<QuerySolutionNode> matchOver(std::unique_ptr<QuerySolutionNode> child) {
    return std::make_unique<MatchNode>(std::move(child),
                                       std::make_unique<AlwaysTrueMatchExpression>());
}

inline std::unique_ptr<QuerySolutionNode> ineligibleOver(std::unique_ptr<QuerySolutionNode> child) {
    return std::make_unique<IneligibleNode>(std::move(child));
}

inline std::unique_ptr<QuerySolution> solutionWithRoot(std::unique_ptr<QuerySolutionNode> root) {
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(root));
    return solution;
}

}  // namespace mongo::parallelism_planner
