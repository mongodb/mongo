/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for the COUNT_SCAN
 * stage. Memory is only tracked when the index is multikey (deduplication is active).
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_90,
 * ]
 */

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const kDocCount = 100;
// 'a' is an array so the index on 'a' is multikey, enabling deduplication in COUNT_SCAN.
const docs = Array.from({length: kDocCount}, (_, i) => ({_id: i, a: [i, i + kDocCount]}));
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({a: 1}));

// Use $match + $count aggregate which optimizes to a COUNT_SCAN when the classic engine is used.
const pipeline = [{$match: {a: {$gte: 0}}}, {$count: "n"}];

// Check if the query uses COUNT_SCAN. This stage is only used by the classic engine.
const preliminaryExplain = coll.explain("executionStats").aggregate(pipeline);
const countScanStages = getAggPlanStages(preliminaryExplain, "COUNT_SCAN");
if (countScanStages.length === 0) {
    jsTest.log.info("Skipping test: query did not use COUNT_SCAN. " + "This stage is only used by the classic engine.");
    coll.drop();
    MongoRunner.stopMongod(conn);
    quit();
}

runMemoryStatsTest({
    db,
    collName,
    commandObj: {
        aggregate: collName,
        pipeline,
        comment: "memory stats count scan test",
        allowDiskUse: false,
        cursor: {batchSize: 1},
    },
    stageName: "COUNT_SCAN",
    // COUNT_SCAN returns a single document, so the cursor is exhausted in
    // the initial batch with no getMore calls.
    expectedNumGetMores: 0,
    // Without getMore calls there are no non-exhausted entries in which to check
    // inUseTrackedMemBytes.
    skipInUseTrackedMemBytesCheck: true,
});

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
