/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $group on a time-series collection (eligible for block processing).
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
import {checkSbeStatus, kSbeDisabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

if (checkSbeStatus(db) == kSbeDisabled) {
    MongoRunner.stopMongod(conn);
    jsTest.log.info("Skipping test as sbe is not enabled.");
    quit();
}

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);

const nBuckets = 50;
const nDocsPerBucket = 200;

const bulk = coll.initializeUnorderedBulkOp();
for (let m = 1; m <= nBuckets; m++) {
    const nPartitions = m % 2 === 0 ? 2 : 100;
    for (let i = 0; i < nDocsPerBucket; i++) {
        bulk.insert({t: new Date(), m, a: i % nPartitions, b: i});
    }
}
assert.commandWorked(bulk.execute());

const pipeline = [{$group: {_id: {y: "$a"}, min: {$min: "$b"}}}];
const pipelineWithLimit = [{$group: {_id: {y: "$a"}, min: {$min: "$b"}}}, {$limit: 10}];

const stageName = "block_group";

// 'forceIncreasedSpilling' should not be enabled for the following tests.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "never"}),
);

// We need the query to run in SBE to use block processing for timeseries.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

{
    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 20},
            comment: "$group on timeseries collection with block processing",
            allowDiskUse: false,
        },
        stageName: stageName,
        expectedNumGetMores: 4,
    });
}

{
    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            cursor: {batchSize: 2},
            comment: "memory stats timeseries $group limit test",
            allowDiskUse: false,
        },
        stageName: stageName,
        expectedNumGetMores: 5,
    });
}

{
    // Set maxMemory low to force spill to disk.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1000,
        }),
    );
    // This parameter is generally true for debug builds, but we would like to turn it off
    // for this test.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "never"}),
    );

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 20},
            comment: "memory stats timeseries $group spilling test",
            allowDiskUse: true,
        },
        stageName: stageName,
        expectedNumGetMores: 4,
        // Since we spill, we don't expect to see
        // inUseTrackedMemBytes populated as it should be 0 on each operation.
        skipInUseTrackedMemBytesCheck: true,
    });
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
