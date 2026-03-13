/**
 * Tests the behavior of explain() when used on a database that does not exist.
 * We should get back an EOF plan
 * @tags: [
 *   # Implicit sharding creates the collection, directly contradicting this test
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # Wrapping in $facet changes the explain output
 *   do_not_wrap_aggregations_in_facets,
 *   # Older versions have different explain behaviour around non-existent DBs
 *   requires_fcv_83,
 * ]
 */

import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase({}));

// Do it twice to make sure the DB does not get created as a side-effect.
for (let i = 0; i < 2; i++) {
    const result = testDB.test.explain().aggregate([]);

    if (isMongos(testDB) || result.explainVersion === "1") {
        assert.eq(result.queryPlanner.winningPlan.stage, "EOF", result);
        assert.eq(result.queryPlanner.winningPlan.type, "nonExistentNamespace", result);
    } else {
        // SBE has a different explain format
        assert.eq(result.queryPlanner.winningPlan.queryPlan.stage, "EOF", result);
        assert.eq(result.queryPlanner.winningPlan.queryPlan.type, "nonExistentNamespace", result);
    }
}
