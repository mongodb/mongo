// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * A tree walker compatible with tree_walker::walk() used to visit the SpecificStats of a
 * BasePlanStageStats tree.
 */
template <bool IsConst, typename... Args>
class PlanStageStatsWalker {
public:
    using Visitor = PlanStatsVisitor<IsConst>;
    using PlanStageStats = tree_walker::MaybeConstPtr<IsConst, BasePlanStageStats<Args...>>;

    PlanStageStatsWalker(Visitor* pre, Visitor* in, Visitor* post)
        : _preVisitor{pre}, _inVisitor{in}, _postVisitor{post} {}

    void preVisit(PlanStageStats stats) {
        if (_preVisitor && stats->specific) {
            stats->specific->acceptVisitor(_preVisitor);
        }
    }

    void inVisit(long count, PlanStageStats stats) {
        if (_inVisitor && stats->specific) {
            stats->specific->acceptVisitor(_inVisitor);
        }
    }

    void postVisit(PlanStageStats stats) {
        if (_postVisitor && stats->specific) {
            stats->specific->acceptVisitor(_postVisitor);
        }
    }

private:
    Visitor* const _preVisitor;
    Visitor* const _inVisitor;
    Visitor* const _postVisitor;
};
}  // namespace mongo
