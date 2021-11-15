/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/query/tree_walker.h"

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
