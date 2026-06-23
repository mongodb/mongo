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

#include "mongo/db/query/query_stats/plan_shape_counters.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/util/assert_util.h"

#include <limits>

namespace mongo::plan_shape_counters {
namespace {

using query_solution_analyzer::Range;
using query_solution_analyzer::StateMachineRule;
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
 * Builds one state machine that recognizes every specific plan shape at once, mirroring
 * 'makeLookupUnwindRule' in engine selection. The machine's states form a trie over the shapes'
 * root-to-leaf stage sequences: shapes that share a leading run of stages share the corresponding
 * states, and each shape's final stage is a match state tagged with that shape.
 *
 * A tree is recognized only when every root-to-leaf path ends in a match state with the same tag,
 * so a plan matches at most one shape by construction; in particular, an OR or SORT_MERGE that
 * has children with two different shapes (e.g. covered and fetched index scans) matches nothing.
 *
 * The trie can be derived from taking the list of plan shapes, reversing the stages within each
 * shape so that the leaves are last, and sorting them so that the shapes with common prefixes are
 * grouped:
 *
 * COLLSCAN
 * FETCH-IXSCAN
 * FETCH-OR-IXSCAN
 * FETCH-SORT-IXSCAN
 * FETCH-SORT_MERGE-IXSCAN
 *
 * Each section of this function indicates the plan shapes it will encode.
 */
StateMachineRule makePlanShapeRule() {
    // Pass ignoreNonEssentialNodes=true to have the state machine only consider
    // nodes that are involved in defining the plan shape.
    StateMachineRule sm(true /* ignoreNonEssentialNodes */);
    // Tags each match state with the shape it completes, so that the matched shape can be
    // recovered through getMatchedTag() once the machine reports a match.
    auto addMatch = [&](int state, PlanShapeCounter shape) {
        sm.addMatchForCounter(state, static_cast<int>(shape));
    };

    const int start = sm.getStartState();

    // COLLSCAN
    addMatch(sm.addState(start, STAGE_COLLSCAN, 0), PlanShapeCounter::kCollscan);

    // Shapes whose root is a FETCH:
    // * FETCH-IXSCAN
    // * FETCH-OR-IXSCAN
    // * FETCH-SORT-IXSCAN
    // * FETCH-SORT_MERGE-IXSCAN
    {
        const int fetch = sm.addState(start, STAGE_FETCH, 0);
        // FETCH-IXSCAN
        addMatch(sm.addState(fetch, STAGE_IXSCAN, 0), PlanShapeCounter::kIxscanFetch);
        // FETCH-OR-IXSCAN
        {
            int s = sm.addState(fetch, STAGE_OR, 0);
            addMatch(sm.addState(s, STAGE_IXSCAN, kAnyChild), PlanShapeCounter::kIxscanOrFetch);
        }
        // FETCH-SORT-IXSCAN
        {
            int s = sm.addState(fetch, kSortTypes, 0);
            addMatch(sm.addState(s, STAGE_IXSCAN, 0), PlanShapeCounter::kIxscanSortFetch);
        }
        // FETCH-SORT_MERGE-IXSCAN
        {
            int s = sm.addState(fetch, STAGE_SORT_MERGE, 0);
            addMatch(sm.addState(s, STAGE_IXSCAN, kAnyChild),
                     PlanShapeCounter::kIxscanSortMergeFetch);
        }
    }

    return sm;
}

// Returns the root of the find portion of the QuerySolution.
const QuerySolutionNode* getFindRoot(const QuerySolution& solution) {
    const QuerySolutionNode* node = solution.root();
    if (!solution.hasExtension()) {
        return node;
    }
    while (node->nodeId() != solution.unextendedRootId()) {
        tassert(11907602,
                "Extension chain ended before reaching the unextended root",
                !node->children.empty());
        node = node->children[0].get();
    }
    return node;
}

}  // namespace

boost::optional<PlanShapeCounter> identifyPlanShapeForCounters(const QuerySolution& qs) {
    auto findRoot = getFindRoot(qs);
    tassert(11907601, "Expected to have a non-null find-layer QSN node", findRoot);

    auto rule = makePlanShapeRule();
    if (!treeSearch(findRoot, rule)) {
        return boost::none;
    }
    return static_cast<PlanShapeCounter>(*rule.getMatchedTag());
}

}  // namespace mongo::plan_shape_counters
