/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $sort with SBE enabled. Due to requires_profiling, this test should not run with sharded
 * clusters.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * requires_fcv_82,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;

// TODO SERVER-104599 Modify test to support classic engine as well.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

const bigStr = Array(1025).toString();  // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

// We are testing SBE sort here, so the stage appears in explain output without the dollar sign.
const stageName = "sort";
const pipeline = [{$sort: {_id: 1, b: -1}}];
const pipelineWithLimit = [{$sort: {_id: 1, b: -1}}, {$limit: nDocs / 10}];
{
    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 10},
            comment: "memory stats sort test",
            allowDiskUse: false
        },
        stageName,
        expectedNumGetMores: 5,
    });
}

{
    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            cursor: {batchSize: 1},
            comment: "memory stats sort limit test",
            allowDiskUse: false
        },
        stageName,
        expectedNumGetMores: 5,
    });
}

{
    // Set maxMemory low to force spill to disk.
    const originalMemoryLimit = assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit}));
    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 10},
            comment: "memory stats sort spilling test",
            allowDiskUse: true
        },
        stageName,
        expectedNumGetMores: 5,
        // Since we spill to disk when adding to the sorter, we don't expect to see inUseMemBytes
        // populated as it should be 0 on each operation.
        skipInUseMemBytesCheck: true,
    });

    // Set maxMemory to back to the original value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimit.was}));
}

//  Clean up.
db[collName].drop();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
