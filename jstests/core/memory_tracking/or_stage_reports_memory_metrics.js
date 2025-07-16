/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for queries that
 * use OrStage for record deduplication.
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
 * ]
 */

import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    // This test is specifically for the classic "or" stage, so don't run the test if the stage
    // might be executed in SBE
    jsTestLog("Skipping test for classic 'or' stage when SBE is fully enabled.");
    quit();
}

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

for (let i = 0; i < 10; ++i) {
    for (let j = 0; j < 10; ++j) {
        assert.commandWorked(coll.insertOne({_id: i * 10 + j, a: i, b: j % 2}));
    }
}

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Create a query that can be solved by two index scans that need to be ORed together.
const pipeline = [
    {$match: {$or: [{a: 5}, {b: 1}]}},
];

runMemoryStatsTest({
    db,
    collName,
    commandObj: {
        aggregate: collName,
        pipeline,
        comment: "memory stats Or stage test",
        allowDiskUse: false,
        cursor: {batchSize: 15}
    },
    stageName: "OR",
    expectedNumGetMores: 3,
    // This stage does not release memory on EOF.
    checkInUseMemBytesResets: false,
});