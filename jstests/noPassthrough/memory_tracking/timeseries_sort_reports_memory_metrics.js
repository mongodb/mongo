/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $sort on a time-series collection.
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

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const timeseriesCollName = jsTestName() + "_timeseries";
const coll = db.getCollection(timeseriesCollName);
db[timeseriesCollName].drop();

// The tests expect that memory metrics appear right after memory is used. Decrease the threshold
// for rate-limiting writes to CurOp. Otherwise, we may report no memory usage if the memory used <
// limit.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 256}));

assert.commandWorked(
    db.createCollection(timeseriesCollName, {
        timeseries: {timeField: "time", metaField: "metadata", granularity: "seconds"},
    }),
);

const bigStr = Array(1025).toString(); // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({
        time: new Date(2025, 7, 15, 12, i),
        metadata: {sensor: "A", idx: i},
        value: i,
        bigStr: bigStr,
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
            allowDiskUse: false,
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
    });
}

{
    const pipelineWithLimit = [{$_internalInhibitOptimization: {}}, {$sort: {time: -1}}, {$limit: nDocs / 10}];

    runMemoryStatsTest({
        db: db,
        collName: timeseriesCollName,
        commandObj: {
            aggregate: timeseriesCollName,
            pipeline: pipelineWithLimit,
            cursor: {batchSize: 1},
            comment: "Memory stats test: Sort with limit on timeseries collection",
            allowDiskUse: false,
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
    });
}

{
    // Set maxMemory low to force spill to disk.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit}),
    );
    runMemoryStatsTest({
        db: db,
        collName: timeseriesCollName,
        commandObj: {
            aggregate: timeseriesCollName,
            pipeline: pipeline,
            cursor: {batchSize: 10},
            comment: "Memory stats spilling test: Sort on timeseries collection",
            allowDiskUse: true,
        },
        stageName: "$sort",
        expectedNumGetMores: 5,
        skipInUseTrackedMemBytesCheck: true,
    });
}

// Clean up.
db[timeseriesCollName].drop();
MongoRunner.stopMongod(conn);
