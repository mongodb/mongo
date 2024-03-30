/**
 * Tests the number of read is correctly bounded during SBE multiplanning. We don't want to reduce
 * the max read bound to 0 because that will effectively disable the trial run tracking for that
 * metric. See SERVER-79088 for more details.
 *
 * TODO SERVER-83887 This entire test can be deleted when we remove the "Classic runtime planning
 * for SBE" feature flag.
 *
 * @tags: [
 *    # This test assumes that SBE is being used for most queries.
 *    featureFlagSbeFull,
 * ]
 */

import {getOptimizer} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = "sbe_multiplanner_db";
const collName = "sbe_multiplanner_coll";

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");

if (FeatureFlagUtil.isPresentAndEnabled(conn, "ClassicRuntimePlanningForSbe")) {
    jsTestLog("Skipping the test because SBE multi planner won't be used");
    MongoRunner.stopMongod(conn);
    quit();
}

const db = conn.getDB(dbName);
const coll = db[collName];

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({b: 0}));
}

// Create two indices to enable multiplanning with two IXSCAN plans. The first index scans field 'a'
// first, which matches no document. The second index scans field 'b' first, which matches all
// documents.
assert.commandWorked(coll.createIndex({
    a: 1,
    b: 1,
}));
assert.commandWorked(coll.createIndex({b: 1}));
const explain = coll.explain("allPlansExecution").aggregate([{
    $match: {
        $or: [{
            a: {$gte: 0},
            b: 0,
        }]
    }
}]);

// Assert that the first index scans zero keys, but this doesn't disable the read bound completely.
// Instead the second index still has at least one number of read budget, so it scans one key.
switch (getOptimizer(explain)) {
    case "classic":
        assert.eq(2, explain.executionStats.allPlansExecution.length, explain);
        assert.eq(0, explain.executionStats.allPlansExecution[0].totalKeysExamined, explain);
        assert.eq(1, explain.executionStats.allPlansExecution[1].totalKeysExamined, explain);
        break;
    case "CQF":
        // TODO SERVER-77719: Implement the assertion for CQF.
        break;
}

MongoRunner.stopMongod(conn);
