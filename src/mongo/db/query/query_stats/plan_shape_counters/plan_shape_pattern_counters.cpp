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

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_pattern_counters.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <array>
#include <limits>
#include <span>
#include <vector>

namespace mongo::plan_shape_counters {
namespace {

using query_solution_analyzer::Range;
using query_solution_analyzer::StateMachine;
using query_solution_analyzer::StateMachineMatcher;
using query_solution_analyzer::treeSearch;

// Node families collapsed by the specific plan shapes: any projection/sort variant matches the
// "PROJECT"/"SORT" step of a shape.
constexpr std::initializer_list<StageType> kProjectTypes = {
    STAGE_PROJECTION_DEFAULT, STAGE_PROJECTION_COVERED, STAGE_PROJECTION_SIMPLE};
constexpr std::initializer_list<StageType> kSortTypes = {STAGE_SORT_DEFAULT, STAGE_SORT_SIMPLE};

// There's no limit on how many children an OR or SORT_MERGE node can have when matching plan
// shapes.
const Range kAnyChild = Range(0, std::numeric_limits<size_t>::max());

/*
 * Represents one node in a plan shape pattern. Can hold multiple stages if the pattern can match
 * more than one node in a step.
 */
struct PathStep {
    PathStep(StageType stage, Range allowedChildren = Range(0))
        : stages{stage}, allowedChildren(allowedChildren) {}
    PathStep(std::initializer_list<StageType> stages, Range allowedChildren = Range(0))
        : stages(stages), allowedChildren(allowedChildren) {}

    std::vector<StageType> stages;
    Range allowedChildren;
};

/*
 * A plan shape pattern to match over a QuerySolution. `counter` is incremented if this pattern
 * is found. This structure is turned into a trie for efficient matching.
 */
struct PlanShape {
    PlanShapeCounter counter;
    std::vector<PathStep> path;
};

const auto& planShapes() {
    static const auto shapes = std::to_array<PlanShape>({
        // Patterns starting with COLLSCAN
        {PlanShapeCounter::kCollscan, {STAGE_COLLSCAN}},

        // Patterns starting with FETCH
        {PlanShapeCounter::kIxscanFetch, {STAGE_FETCH, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanOrFetch, {STAGE_FETCH, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanSortFetch, {STAGE_FETCH, kSortTypes, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanSortMergeFetch,
         {STAGE_FETCH, STAGE_SORT_MERGE, {STAGE_IXSCAN, kAnyChild}}},

        // Patterns starting with OR
        {PlanShapeCounter::kIxscanFetchOr, {STAGE_OR, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},

        // Patterns starting with SORT_MERGE
        {PlanShapeCounter::kIxscanFetchSortMerge,
         {STAGE_SORT_MERGE, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},

        // Patterns starting with PROJECT
        {PlanShapeCounter::kCollscanProject, {kProjectTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanProject, {kProjectTypes, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanFetchProject, {kProjectTypes, STAGE_FETCH, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanOrFetchProject,
         {kProjectTypes, STAGE_FETCH, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanSortFetchProject,
         {kProjectTypes, STAGE_FETCH, kSortTypes, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanSortMergeFetchProject,
         {kProjectTypes, STAGE_FETCH, STAGE_SORT_MERGE, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanOrProject, {kProjectTypes, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanFetchOrProject,
         {kProjectTypes, STAGE_OR, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanSortMergeProject,
         {kProjectTypes, STAGE_SORT_MERGE, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanFetchSortMergeProject,
         {kProjectTypes, STAGE_SORT_MERGE, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},
        {PlanShapeCounter::kCollscanSortProject, {kProjectTypes, kSortTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanFetchSortProject,
         {kProjectTypes, kSortTypes, STAGE_FETCH, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanOrFetchSortProject,
         {kProjectTypes, kSortTypes, STAGE_FETCH, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanFetchOrSortProject,
         {kProjectTypes, kSortTypes, STAGE_OR, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},
        {PlanShapeCounter::kCollscanProjectSortProject,
         {kProjectTypes, kSortTypes, kProjectTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanProjectSortProject,
         {kProjectTypes, kSortTypes, kProjectTypes, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanFetchProjectSortProject,
         {kProjectTypes, kSortTypes, kProjectTypes, STAGE_FETCH, STAGE_IXSCAN}},

        // Patterns starting with SORT
        {PlanShapeCounter::kCollscanSort, {kSortTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanFetchSort, {kSortTypes, STAGE_FETCH, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanOrFetchSort,
         {kSortTypes, STAGE_FETCH, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanFetchOrSort,
         {kSortTypes, STAGE_OR, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},
        {PlanShapeCounter::kCollscanProjectSort, {kSortTypes, kProjectTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanProjectSort, {kSortTypes, kProjectTypes, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanFetchProjectSort,
         {kSortTypes, kProjectTypes, STAGE_FETCH, STAGE_IXSCAN}},
        {PlanShapeCounter::kIxscanOrFetchProjectSort,
         {kSortTypes, kProjectTypes, STAGE_FETCH, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanOrProjectSort,
         {kSortTypes, kProjectTypes, STAGE_OR, {STAGE_IXSCAN, kAnyChild}}},
        {PlanShapeCounter::kIxscanFetchOrProjectSort,
         {kSortTypes, kProjectTypes, STAGE_OR, {STAGE_FETCH, kAnyChild}, STAGE_IXSCAN}},
        {PlanShapeCounter::kCollscanSortProjectSort,
         {kSortTypes, kProjectTypes, kSortTypes, STAGE_COLLSCAN}},
        {PlanShapeCounter::kIxscanFetchSortProjectSort,
         {kSortTypes, kProjectTypes, kSortTypes, STAGE_FETCH, STAGE_IXSCAN}},
    });
    static_assert(std::tuple_size_v<decltype(shapes)> == kNumPlanShapeCounters,
                  "planShapes() must define exactly one path per plan shape counter");
    return shapes;
}

/*
 * Adds a given `path` to the StateMachine, starting at the `sm` node in the trie. At the
 * leaf of the trie, we place the `counter` tag.
 */
void addShape(StateMachine& sm,
              std::span<const PathStep> path,
              int state,
              PlanShapeCounter counter) {
    if (path.empty()) {
        sm.addMatchForCounter(state, static_cast<int>(counter));
        return;
    }
    const PathStep& step = path.front();
    // We may have different types of nodes to match at this step. We need to add all of them to
    // the trie.
    for (StageType stage : step.stages) {
        const int next = sm.addOrGetState(state, stage, step.allowedChildren);
        addShape(sm, path.subspan(1), next, counter);
    }
}

/*
 * Compiles 'planShapes()' into one state machine that recognizes every plan shape at once.
 * The machine's states form a trie over the shapes' root-to-leaf stage sequences: shapes
 * that share a leading run of stages share the corresponding states, and each shape's
 * final stage is a match state tagged with that shape.
 *
 * A tree is recognized only when every root-to-leaf path ends in a match state with the same tag,
 * so a plan matches at most one shape by construction; in particular, an OR or SORT_MERGE that has
 * children with two different shapes (e.g. covered and fetched index scans) matches nothing.
 */
StateMachine makePlanShapeRule() {
    StateMachine sm;

    for (const auto& shape : planShapes()) {
        addShape(sm, shape.path, sm.getStartState(), shape.counter);
    }
    // Since this StateMachine is `static const`, we shrink to fit to avoid taking unnecessary
    // space for the entire lifetime of the process.
    sm.shrinkToFit();

    const size_t sizeBytes = sm.sizeInBytes();
    static constexpr size_t kMaxSizeBytes = 128 * 1024;
    tassert(11907604,
            str::stream() << "Plan shape rule state machine size: " << sizeBytes
                          << " bytes exceeds limit of " << kMaxSizeBytes << " bytes",
            sizeBytes <= kMaxSizeBytes);

    return sm;
}

// Returns the root of the find portion of the QuerySolution.
const QuerySolutionNode* getFindRoot(const QuerySolution& solution) {
    const QuerySolutionNode* node = solution.root();
    if (!solution.hasExtension()) {
        return node;
    }
    // The unextended root can be removed by some optimizations, so we can't
    // search for the exact node ID.
    while (node->nodeId() > solution.unextendedRootId()) {
        tassert(11907602,
                "Extension chain ended before reaching the unextended root",
                !node->children.empty());
        const QuerySolutionNode* child = node->children[0].get();
        tassert(11907603,
                "Node ids do not strictly decrease down the extension branch",
                child->nodeId() < node->nodeId());
        node = child;
    }
    return node;
}

}  // namespace

boost::optional<PlanShapeCounter> identifyPlanShapeForCounters(const QuerySolution& qs) {
    static const StateMachine machine = makePlanShapeRule();

    auto findRoot = getFindRoot(qs);
    tassert(11907601, "Expected to have a non-null find-layer QSN node", findRoot);

    // Pass ignoreNonEssentialNodes=true to have the state machine only consider
    // nodes that are involved in defining the plan shape.
    StateMachineMatcher matcher(machine, true /* ignoreNonEssentialNodes */);
    if (!treeSearch(findRoot, matcher)) {
        return boost::none;
    }
    return static_cast<PlanShapeCounter>(*matcher.getMatchedTag());
}

}  // namespace mongo::plan_shape_counters
