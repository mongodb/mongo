// Tests that the planner correctly deduplicates query solutions.
//
// Implicit index creation may prevent duplicate plans from being generated. For e.g. they may
// cause the query to hit an $or enumeration limit before enumerating the same plan twice.
// @tags: [
//   assumes_no_implicit_index_creation,
//   # Incompatible with stepdowns because the test modifies an internal knob.
//   does_not_support_stepdowns,
//   # setParameter not permitted with security tokens
//   not_allowed_with_signed_security_token,
//   # Uses 'internalQueryDeduplicateQuerySolutions' which doesn't exist on older versions.
//   requires_fcv_80,
//   multiversion_incompatible,
// ]

import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/analyze_plan.js";
import {tojsonOnelineSortKeys} from "jstests/libs/golden_test.js";
import {runWithParamsAllNodes} from "jstests/libs/optimizer_utils.js";

function runWithoutPlanDeduplication(fn) {
    const disableDedup = [{key: "internalQueryDeduplicateQuerySolutions", value: false}];
    return runWithParamsAllNodes(db, disableDedup, fn);
}

const coll = db[jsTestName()];

function setupCollection(indexes) {
    coll.drop();
    coll.insert({t: 1, m: "foo", c: 1});

    assert.commandWorked(coll.createIndexes(indexes));
}

function getDuplicatePlans(filter) {
    const explainRoot = coll.find(filter).explain();
    print("explain", tojson(explainRoot));

    const plans = [getWinningPlanFromExplain(explainRoot), ...getRejectedPlans(explainRoot)].map(
        p => tojsonOnelineSortKeys(p));

    const counts = {};
    plans.forEach(p => counts[p] = (counts[p] || 0) + 1);

    return Object.entries(counts).filter(([_, count]) => count > 1);
}

function assertUniqueSolutions({filter, indexes}) {
    setupCollection(indexes);

    // Assert that the test case is actually generating duplicate solutions.
    runWithoutPlanDeduplication(() => {
        const duplicatePlans = getDuplicatePlans(filter);
        assert.neq(duplicatePlans.length, 0);
    });

    // Assert that we successfully deduplicate the solutions.
    const duplicatePlans = getDuplicatePlans(filter);
    for (const [plan, _] of duplicatePlans) {
        print("Duplicate plan:", plan);
    }
    assert.eq(duplicatePlans.length, 0);
}

// Produces one duplicate plan.
assertUniqueSolutions({
    filter: {t: 1, $or: [{t: {$gte: 1}}, {t: {$lte: 1}}]},
    indexes: [{t: 1}, {t: -1}],
});

// Produces three duplicate plans before hitting the $or enumeration limit.
assertUniqueSolutions({
    filter: {
        "t": 1,
        "m": "foo",
        $or: [{"t": {$gte: 1}, c: {$lt: 3}}, {"t": {$lte: 1}, c: {$lt: 3}}],
    },
    indexes: [
        {t: 1, m: 1, c: 1},
        {m: 1, t: 1, c: 1},
        {c: 1, t: 1, m: 1},
    ],
});
