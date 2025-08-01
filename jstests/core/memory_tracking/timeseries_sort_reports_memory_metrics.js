/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $sort on a timeseries collection. Due to requires_profiling, this test should not run with
 * sharded clusters.
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
 * requires_timeseries,
 * assumes_against_mongod_not_mongos,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const timeseriesCollName = jsTestName() + "_timeseries";
const coll = db.getCollection(timeseriesCollName);
db[timeseriesCollName].drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;

assert.commandWorked(db.createCollection(
    timeseriesCollName,
    {timeseries: {timeField: "time", metaField: "metadata", granularity: "seconds"}}));

const bigStr = Array(1025).toString();  // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({
        time: new Date(2025, 7, 15, 12, i),
        metadata: {sensor: "A", idx: i},
        value: i,
        bigStr: bigStr
    });
}
assert.commandWorked(bulk.execute());

const pipeline = [{$_internalInhibitOptimization: {}}, {$sort: {time: -1}}];

{
    runMemoryStatsTest({
        db: db,
        collName: timeseriesCollName,
        commandObj: {
            aggregate: timeseriesCollName,
            pipeline: pipeline,
            cursor: {batchSize: 10},
            comment: "Memory stats test: Sort on timeseries collection",
            allowDiskUse: false
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
    });
}

{
    const pipelineWithLimit =
        [{$_internalInhibitOptimization: {}}, {$sort: {time: -1}}, {$limit: nDocs / 10}];

    runMemoryStatsTest({
        db: db,
        collName: timeseriesCollName,
        commandObj: {
            aggregate: timeseriesCollName,
            pipeline: pipelineWithLimit,
            cursor: {batchSize: 1},
            comment: "Memory stats test: Sort with limit on timeseries collection",
            allowDiskUse: false
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
    });
}

{
    // Set maxMemory low to force spill to disk.
    const originalMemoryLimit = assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit}));
    runMemoryStatsTest({
        db: db,
        collName: timeseriesCollName,
        commandObj: {
            aggregate: timeseriesCollName,
            pipeline: pipeline,
            cursor: {batchSize: 10},
            comment: "Memory stats spilling test: Sort on timeseries collection",
            allowDiskUse: true
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
        skipInUseMemBytesCheck: true,
    });
    // Restore memory limit.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimit.was}));
}

// Clean up.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
