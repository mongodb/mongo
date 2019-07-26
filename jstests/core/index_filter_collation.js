/**
 * Test that index filters are applied with the correct collation.
 * @tags: [
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   # Needs to create a collection with a collation.
 *   assumes_no_implicit_collection_creation_after_drop
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanStages.

const collName = "index_filter_collation";
const coll = db[collName];

const caseInsensitive = {
    locale: "fr",
    strength: 2
};
coll.drop();
assert.commandWorked(db.createCollection(collName, {collation: caseInsensitive}));

function checkIndexFilterSet(explain, shouldBeSet) {
    if (explain.queryPlanner.winningPlan.shards) {
        for (let shard of explain.queryPlanner.winningPlan.shards) {
            assert.eq(shard.indexFilterSet, shouldBeSet);
        }
    } else {
        assert.eq(explain.queryPlanner.indexFilterSet, shouldBeSet);
    }
}

// Now create an index filter on a query with no collation specified.
assert.commandWorked(coll.createIndexes([{x: 1}, {x: 1, y: 1}]));
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {"x": 3}, indexes: [{x: 1, y: 1}]}));

const listFilters = assert.commandWorked(db.runCommand({planCacheListFilters: collName}));
assert.eq(listFilters.filters.length, 1);
assert.eq(listFilters.filters[0].query, {x: 3});
assert.eq(listFilters.filters[0].indexes, [{x: 1, y: 1}]);

// Create an index filter on a query with the default collation specified.
assert.commandWorked(db.runCommand({
    planCacheSetFilter: collName,
    query: {"x": 3},
    collation: caseInsensitive,
    indexes: [{x: 1}]
}));

// Although these two queries would run with the same collation, they have different "shapes"
// so we expect there to be two index filters present.
let res = assert.commandWorked(db.runCommand({planCacheListFilters: collName}));
assert.eq(res.filters.length, 2);

// One of the filters should only be applied to queries with the "fr" collation
// and use the {x: 1} index.
assert(res.filters.some((filter) => filter.hasOwnProperty("collation") &&
                            filter.collation.locale === "fr" &&
                            friendlyEqual(filter.indexes, [{x: 1}])));

// The other should not have any collation, and allow the index {x: 1, y: 1}.
assert(res.filters.some((filter) => !filter.hasOwnProperty("collation") &&
                            friendlyEqual(filter.indexes, [{x: 1, y: 1}])));

function assertIsIxScanOnIndex(winningPlan, keyPattern) {
    const ixScans = getPlanStages(winningPlan, "IXSCAN");
    assert.gt(ixScans.length, 0);
    assert.eq(ixScans[0].keyPattern, keyPattern);
}

// Run the queries and be sure the correct indexes are used.
let explain = coll.find({x: 3}).explain();
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(explain.queryPlanner.winningPlan, {x: 1, y: 1});

// Run the queries and be sure the correct indexes are used.
explain = coll.find({x: 3}).collation(caseInsensitive).explain();
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(explain.queryPlanner.winningPlan, {x: 1});
})();
