/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $graphLookup.
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

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

const docCount = 3;
for (let i = 0; i < docCount; ++i) {
    assert.commandWorked(coll.insertOne({_id: i, to: [i - 1, i, i + 1]}));
}

let pipeline = [
    {
        $graphLookup: {
            from: collName,
            startWith: "$_id",
            connectFromField: "to",
            connectToField: "_id",
            as: "output",
        }
    },
    {$unwind: "$output"}
];

runMemoryStatsTest({
    db,
    collName,
    commandObj: {
        aggregate: collName,
        pipeline,
        comment: "memory stats graphLookup test",
        allowDiskUse: false,
        cursor: {batchSize: 1}
    },
    stageName: "$graphLookup",
    expectedNumGetMores: 8
});
