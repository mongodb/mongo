/**
 * Test that hidden indexes work as expected when corresponding index filters are applied.
 *
 * - When a query with a shape matching an index filter is executed, the index referenced by
 *   the filter is *not* used to answer the query if that index is currently hidden.
 * - If an alternative non-hidden index in the index filter is available, it is used to answer the
 *   query. Otherwise, it results in a COLLSCAN.
 * - Un-hiding the index restores the index filter behaviour.
 * - It is legal to set an index filter on a hidden index, but the index will not actually be
 *   used until it is made visible.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheListFilters,
 *   # planCacheSetFilter.
 *   not_allowed_with_signed_security_token,
 *   # Command 'planCacheSetFilter' may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # In some scenarios this test asserts that a collection scan is the chosen plan.
 *   assumes_no_implicit_index_creation,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 * ]
 */

import {getPlanStages, getWinningPlan, isCollscan} from "jstests/libs/analyze_plan.js";

const collName = 'hidden_indexes_remain_visible_in_index_filters';
db[collName].drop();
const coll = db[collName];

assert.commandWorked(coll.insert([{a: 1, b: 1, c: 1}, {a: 2, b: 2, c: 2}]));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));

const queryShape = {
    query: {a: {$gt: 0}, b: {$gt: 0}},
    sort: {a: -1, b: -1},
    projection: {_id: 0, a: 1}
};

// Ensure the filters for the given query shape exist.
function ensureFilterExistsByQueryShape(queryShape) {
    const res = assert.commandWorked(coll.runCommand('planCacheListFilters'));
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    const filter = res.filters.find(function(obj) {
        return tojson(obj.query) === tojson(queryShape.query) &&
            tojson(obj.projection) === tojson(queryShape.projection) &&
            tojson(obj.sort) === tojson(queryShape.sort);
    });

    assert(filter, `Index filter not found for query shape ${tojson(queryShape)}`);
}

// If non-null 'idxName' is given, the given index 'idxName' is expected to be used for the given
// 'queryShape'. Otherwise, a COLLSCAN stage is expected.
function validateIxscanOrCollscanUsed(queryShape, idxName) {
    const explain = assert.commandWorked(
        coll.find(queryShape.query, queryShape.projection).sort(queryShape.sort).explain());

    if (idxName) {
        // Expect the given index was used.
        const ixScanStage = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN")[0];
        assert(ixScanStage, `Index '${idxName}' was not used.`);
        assert.eq(ixScanStage.indexName, idxName, `Index '${idxName}' was not used.`);
    } else {
        // Expect a COLLSCAN stage.
        assert(isCollscan(db, explain));
    }
}

// Add index filters for simple query.
assert.commandWorked(coll.runCommand('planCacheSetFilter', {
    query: queryShape.query,
    sort: queryShape.sort,
    projection: queryShape.projection,
    indexes: [{a: 1}, {a: 1, b: 1}]
}));
ensureFilterExistsByQueryShape(queryShape);

// The index should be used as usual if it's not hidden.
validateIxscanOrCollscanUsed(queryShape, "a_1_b_1");

// Hide index 'a_1_b_1'. Expect the other unhidden index 'a_1' will be used.
assert.commandWorked(coll.hideIndex("a_1_b_1"));
validateIxscanOrCollscanUsed(queryShape, "a_1");

// Hide index 'a_1' as well, at which point there are no available indexes remaining in the index
// filter. We do not expect the planner to use the 'a_1_b_1_c_1' index since it is outside the
// filter, so we should see a COLLSCAN instead.
assert.commandWorked(coll.hideIndex("a_1"));
validateIxscanOrCollscanUsed(queryShape, null);

// Ensure the index filters in the plan cache won't be affected by hiding the corresponding indexes.
ensureFilterExistsByQueryShape(queryShape);

// Ensure that unhiding the indexes can restore the index filter behaviour.
assert.commandWorked(coll.unhideIndex("a_1"));
validateIxscanOrCollscanUsed(queryShape, "a_1");
assert.commandWorked(coll.unhideIndex("a_1_b_1"));
validateIxscanOrCollscanUsed(queryShape, "a_1_b_1");

// Ensure that it is legal to set an index filter on a hidden index, but the index will not actually
// be used until it is made visible.
assert.commandWorked(coll.hideIndex("a_1"));

// Set index filters on a hidden index.
assert.commandWorked(coll.runCommand('planCacheSetFilter', {
    query: queryShape.query,
    sort: queryShape.sort,
    projection: queryShape.projection,
    indexes: [{a: 1}]
}));
ensureFilterExistsByQueryShape(queryShape);

// The hidden index 'a_1' cannot be used even though it's in the index filter.
validateIxscanOrCollscanUsed(queryShape, null);

// Unhiding the index should make it able to be used.
assert.commandWorked(coll.unhideIndex("a_1"));
validateIxscanOrCollscanUsed(queryShape, "a_1");
