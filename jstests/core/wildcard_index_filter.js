/**
 * Test that $** indexes obey index filter rules.
 *
 * Does not support stepdowns, because the stepdown/kill_primary passthroughs will reject commands
 * that may return different values after a failover; in this case, 'planCacheClearFilters'.
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");  // For 'isMongos()'.

const coll = db.wildcard_index_filter;

// Utility function to list index filters.
function getFilters() {
    const res = assert.commandWorked(coll.runCommand('planCacheListFilters'));
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    return res.filters;
}

// Sets an index filter given a query shape then confirms that the expected index was used to
// answer a query.
function assertExpectedIndexAnswersQueryWithFilter(
    filterQuery, filterIndexes, query, expectedIndexName, hint) {
    // Clear existing cache filters.
    assert.commandWorked(coll.runCommand('planCacheClearFilters'), 'planCacheClearFilters failed');

    // Make sure that the filter is set correctly.
    assert.commandWorked(
        coll.runCommand('planCacheSetFilter', {query: filterQuery, indexes: filterIndexes}));
    assert.eq(1,
              getFilters().length,
              'no change in query settings after successfully setting index filters');

    // Check that expectedIndex index was used over another index.
    let explain;
    if (hint === undefined) {
        explain = assert.commandWorked(coll.find(query).explain('executionStats'));
    } else {
        explain = assert.commandWorked(coll.find(query).hint(hint).explain('executionStats'));
    }

    const winningPlan = getWinningPlan(explain.queryPlanner);
    const planStages = getPlanStages(winningPlan, 'IXSCAN');

    if (FixtureHelpers.isMongos(db)) {
        assert.gte(planStages.length, 1, explain);
    } else {
        // If we're not running on a sharded cluster, there should be exactly one IXSCAN stage.
        assert.eq(planStages.length, 1, explain);
    }

    for (const stage of planStages) {
        assert(stage.hasOwnProperty('indexName'), stage);
        assert.eq(stage.indexName, expectedIndexName, stage);
    }
}

const indexWildcard = {
    "$**": 1
};
const indexA = {
    "a": 1
};
assert.commandWorked(coll.createIndex(indexWildcard));
assert.commandWorked(coll.createIndex(indexA));

assert.commandWorked(coll.insert({a: "a"}));

// Filtering on $** index. $** index is used over another index.
assertExpectedIndexAnswersQueryWithFilter({a: "a"}, [indexWildcard], {a: "a"}, "$**_1");

// Filtering on regular index. $** index is not used over another index.
assertExpectedIndexAnswersQueryWithFilter({a: "a"}, [indexA], {a: "a"}, "a_1");

assert.commandWorked(coll.insert({a: "a", b: "b"}));

const indexAB = {
    "a": 1,
    "b": 1
};
assert.commandWorked(coll.createIndex(indexAB));

// Filtering on $** index. $** index is used over another index for compound query.
assertExpectedIndexAnswersQueryWithFilter(
    {a: "a", b: "b"}, [indexWildcard], {a: "a", b: "b"}, "$**_1");

// Filtering on regular compound index. Check that $** index is not used over another index
// for compound query.
assertExpectedIndexAnswersQueryWithFilter({a: "a", b: "b"}, [indexAB], {a: "a", b: "b"}, "a_1_b_1");

// Filtering on $** index while hinting on another index. Index filter is prioritized.
assertExpectedIndexAnswersQueryWithFilter({a: "a"}, [indexWildcard], {a: "a"}, "$**_1", indexA);

// Filtering on regular index while hinting on $** index. Index filter is prioritized.
assertExpectedIndexAnswersQueryWithFilter({a: "a"}, [indexA], {a: "a"}, "a_1", indexWildcard);

// Index filter for $** index does not apply when query does not match filter query shape.
assertExpectedIndexAnswersQueryWithFilter({b: "b"}, [indexWildcard], {a: "a"}, "a_1", indexA);

const indexAWildcard = {
    "a.$**": 1
};
assert.commandWorked(coll.createIndex(indexAWildcard));

// Filtering on a path specified $** index. Check that the $** is used over other indices.
assertExpectedIndexAnswersQueryWithFilter({a: "a"}, [indexAWildcard], {a: "a"}, "a.$**_1");
})();
