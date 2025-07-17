/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $bucketAuto using the classic engine.
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

const stageName = "$bucketAuto";
const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Set up test collection.
const docs = [];
for (let i = 1; i <= 100; i++) {
    docs.push({
        value: i,
        category: i % 10 === 0 ? "decade" : "regular",
        group: Math.floor(i / 25) + 1  // Creates 4 groups (1-25, 26-50, 51-75, 76-100).
    });
}
assert.commandWorked(coll.insertMany(docs));

{
    const pipeline = [{
        $bucketAuto: {
            groupBy: "$value",
            buckets: 5,
            output:
                {"count": {$sum: 1}, "valueList": {$push: "$value"}, "avgValue": {$avg: "$value"}}
        }
    }];
    jsTestLog("Running basic pipeline test : " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats bucketAuto test",
            allowDiskUse: false,
        },
        stageName,
        expectedNumGetMores: 5,
    });
}

{
    const pipeline = [
        {
            $bucketAuto: {
                groupBy: "$value",
                buckets: 5,
                output: {
                    "count": {$sum: 1},
                    "valueList": {$push: "$value"},
                    "avgValue": {$avg: "$value"}
                }
            }
        },
        {$limit: 1}
    ];
    jsTestLog("Running pipeline with $limit : " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats bucketAuto with limit test",
            allowDiskUse: false
        },
        stageName,
        expectedNumGetMores: 1,
        skipInUseMemBytesCheck: true,  // $limit will force execution to stop early, so
                                       // inUseMemBytes may not appear in CurOp.
    });
}

{
    const pipeline = [
        {
            $bucketAuto: {
                groupBy: "$value",
                buckets: 5,
                output: {
                    "count": {$sum: 1},
                    "valueList": {$push: "$value"},
                    "avgValue": {$avg: "$value"}
                }
            }
        },
    ];
    jsTestLog("Running pipeline that will spill : " + tojson(pipeline));

    // Set a low memory limit to force spilling to disk.
    const originalMemoryLimit =
        assert
            .commandWorked(db.adminCommand(
                {getParameter: 1, internalDocumentSourceBucketAutoMaxMemoryBytes: 1}))
            .internalDocumentSourceBucketAutoMaxMemoryBytes;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalDocumentSourceBucketAutoMaxMemoryBytes: 100}));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats bucketAuto with spilling test",
            allowDiskUse: true
        },
        stageName,
        expectedNumGetMores: 5
    });

    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalDocumentSourceBucketAutoMaxMemoryBytes: originalMemoryLimit}));
}

// Clean up.
db[collName].drop();
