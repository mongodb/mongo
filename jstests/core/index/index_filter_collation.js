/**
 * Test that index filters are applied with the correct collation.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheListFilters,
 *   # planCacheSetFilter.
 *   not_allowed_with_signed_security_token,
 *   # Needs to create a collection with a collation.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 * ]
 */
import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";

const collName = "index_filter_collation";
const coll = db[collName];

// Flag indicating if index filter commands are running through the query settings interface.
var isIndexFiltersToQuerySettings = TestData.isIndexFiltersToQuerySettings || false;

const caseInsensitive = {
    locale: "fr",
    strength: 2
};
coll.drop();
// Create the collection with a default collation. This test also ensures that index filters will
// be applied before the resolution of collection's collation if users did not specify a collation
// for the queries.
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

// Now create an index filter on a query with no collation specified. The index filter does not
// inherit the collection's default collation.
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

// The index filters with projection are for testing distinct commands.
assert.commandWorked(db.runCommand({
    planCacheSetFilter: collName,
    query: {"x": 5},
    projection: {"_id": 1},
    indexes: [{x: 1, y: 1}]
}));

assert.commandWorked(db.runCommand({
    planCacheSetFilter: collName,
    query: {"x": 5},
    projection: {"_id": 1},
    collation: caseInsensitive,
    indexes: [{x: 1}]
}));

// Although these two queries would run with the same collation, they have different "shapes"
// so we expect there to be four index filters present.
let res = assert.commandWorked(db.runCommand({planCacheListFilters: collName}));
assert.eq(res.filters.length, 4);

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

// Run a query that does not specify the collation, and therefore will inherit the default
// collation. Index filters are applied prior to resolving the collation. Therefore, the index
// filter without a collation should apply to this query.
let explain = coll.find({x: 3}).explain();
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(getWinningPlan(explain.queryPlanner), {x: 1, y: 1});

// When the query specifies the collation, the index filter that also specifies the collation should
// apply.
explain = coll.find({x: 3}).collation(caseInsensitive).explain();
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(getWinningPlan(explain.queryPlanner), {x: 1});

if (isIndexFiltersToQuerySettings) {
    // Query settings can't be used to substitute index filters for distinct commands. Configuring
    // query settings requires providing full query shape. For 'find' one could build it from
    // parameters passed into planCacheSetFilter. For 'distinct' query 'key' field is missing.
    quit();
}

// Ensure distinct commands behave correctly and consistently with the find commands.
explain = coll.explain().distinct("_id", {x: 3});
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(getWinningPlan(explain.queryPlanner), {x: 1, y: 1});

explain = coll.explain().distinct("_id", {x: 3}, {collation: caseInsensitive});
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(getWinningPlan(explain.queryPlanner), {x: 1});
