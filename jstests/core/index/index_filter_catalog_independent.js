/**
 * Test that index filters are applied regardless of catalog changes. Intended to reproduce
 * SERVER-33303.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheListFilters,
 *   # planCacheSetFilter.
 *   not_allowed_with_signed_security_token,
 *   # This test performs queries with index filters set up. Since index filters are local to a
 *   # mongod, and do not replicate, this test must issue all of its commands against the same node.
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   assumes_balancer_off,
 * ]
 */
import {
    getOptimizer,
    getPlanStages,
    getQueryPlanners,
    getWinningPlan,
    getWinningPlanFromExplain,
    isCollscan
} from "jstests/libs/analyze_plan.js";

const collName = "index_filter_catalog_independent";
const coll = db[collName];
coll.drop();

/*
 * Check that there's one index filter on the given query which allows only 'indexes'.
 */
function assertOneIndexFilter(query, indexes) {
    let res = assert.commandWorked(db.runCommand({planCacheListFilters: collName}));
    assert.eq(res.filters.length, 1);
    assert.eq(res.filters[0].query, query);
    assert.eq(res.filters[0].indexes, indexes);
}

function assertIsIxScanOnIndex(explain, keyPattern) {
    switch (getOptimizer(explain)) {
        case "classic": {
            let winningPlan = getWinningPlan(explain.queryPlanner);
            const ixScans = getPlanStages(winningPlan, "IXSCAN");
            assert.gt(ixScans.length, 0);
            ixScans.every((ixScan) => assert.eq(ixScan.keyPattern, keyPattern));

            const collScans = getPlanStages(winningPlan, "COLLSCAN");
            assert.eq(collScans.length, 0);
            break;
        }
        case "CQF": {
            // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
            // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
            assert(isCollscan(db, getWinningPlanFromExplain(explain)));
            break;
        }
    }
}

function checkIndexFilterSet(explain, shouldBeSet) {
    getQueryPlanners(explain).forEach((queryPlanner) => {
        // When field "indexFilterSet" is not set (indicated as value 'undefined'), convert it to
        // false.
        assert.eq(!!queryPlanner.indexFilterSet, shouldBeSet, explain);
    });
}

// Verify that no index filter on "find" command is applied when no index filters are set on the
// collection.
let explain = assert.commandWorked(coll.find({x: 3}).explain());
checkIndexFilterSet(explain, false);

assert.commandWorked(coll.createIndexes([{x: 1}, {x: 1, y: 1}]));
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {"x": 3}, indexes: [{x: 1, y: 1}]}));
assertOneIndexFilter({x: 3}, [{x: 1, y: 1}]);

explain = assert.commandWorked(coll.find({x: 3}).explain());
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(explain, {x: 1, y: 1});

// Drop an index. The filter should not change.
assert.commandWorked(coll.dropIndex({x: 1, y: 1}));
assertOneIndexFilter({x: 3}, [{x: 1, y: 1}]);

// The {x: 1} index _could_ be used, but should not be considered because of the filter.
// Since we dropped the {x: 1, y: 1} index, a COLLSCAN must be used.
explain = coll.find({x: 3}).explain();
checkIndexFilterSet(explain, true);
assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

// Create another index. This should not change whether the index filter is applied.
assert.commandWorked(coll.createIndex({x: 1, z: 1}));
explain = assert.commandWorked(coll.find({x: 3}).explain());
checkIndexFilterSet(explain, true);
assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

// Changing the catalog and then setting an index filter should not result in duplicate entries.
assert.commandWorked(coll.createIndex({x: 1, a: 1}));
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {"x": 3}, indexes: [{x: 1, y: 1}]}));
assertOneIndexFilter({x: 3}, [{x: 1, y: 1}]);

// Recreate the {x: 1, y: 1} index and be sure that it's still used.
assert.commandWorked(coll.createIndexes([{x: 1}, {x: 1, y: 1}]));
assertOneIndexFilter({x: 3}, [{x: 1, y: 1}]);

explain = assert.commandWorked(coll.find({x: 3}).explain());
checkIndexFilterSet(explain, true);
assertIsIxScanOnIndex(explain, {x: 1, y: 1});
