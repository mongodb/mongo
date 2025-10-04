/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * that use the SORT_MERGE PlanStage. Since the SORT_MERGE stage does not run on the merging node,
 * sharded tests are omitted.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   requires_fcv_82,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

if (checkSbeFullyEnabled(db)) {
    // This test is specifically for the classic SORT_MERGE stage, so don't run the test if the
    // stage might be executed in SBE.
    MongoRunner.stopMongod(conn);
    jsTest.log.info("Skipping test for classic 'SORT_MERGE' stage when SBE is fully enabled.");
    quit();
}

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insertOne({_id: i, a: i % 2, b: i, c: i}));
}

assert.commandWorked(coll.createIndex({a: 1, c: 1}));
assert.commandWorked(coll.createIndex({b: 1, c: 1}));

{
    const pipeline = [{$match: {$or: [{a: 1}, {b: 1}]}}, {$sort: {c: -1}}];
    runMemoryStatsTest({
        db,
        collName,
        commandObj: {
            aggregate: collName,
            pipeline,
            comment: "memory stats mergesort test",
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        stageName: "SORT_MERGE",
        // There are 5 `getMore` operations because the pipeline filters down the original
        // 10 documents to 5 matching documents via $match, and a batch size of 1 fetches
        // each document sequentially.
        expectedNumGetMores: 5,
        // This stage does not release memory on EOF.
        checkInUseTrackedMemBytesResets: false,
    });
}

{
    const pipelineWithLimit = [{$match: {$or: [{a: 1}, {b: 1}]}}, {$sort: {c: -1}}, {$limit: 3}];
    runMemoryStatsTest({
        db,
        collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            comment: "memory stats mergesort limit test",
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        stageName: "SORT_MERGE",
        expectedNumGetMores: 2,
        checkInUseTrackedMemBytesResets: false,
    });
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
