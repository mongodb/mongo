// Tests that serverStatus() updates spilling statistics while the query is running instead of after
// the query finishes.
//
// @tags: [
//   requires_persistence,
// ]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    getPlanStages,
    getQueryPlanner,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {setParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const collName = "spill_to_disk_server_status";
const coll = db[collName];
coll.drop();
const foreignCollName = "spill_to_disk_server_status_foreign";
const foreignColl = db[foreignCollName];
foreignColl.drop();
const isSbeEnabled = checkSbeFullyEnabled(db);

// Set up relevant query knobs so that the query will spill for every document.
// No batching in document source cursor
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorInitialBatchSize", 1));
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorBatchSizeBytes", 1));
// Spilling memory threshold for $sort
assert.commandWorked(setParameter(db, "internalQueryMaxBlockingSortMemoryUsageBytes", 1));
// Spilling memory threshold for $group
assert.commandWorked(setParameter(db, "internalDocumentSourceGroupMaxMemoryBytes", 1));
assert.commandWorked(
    setParameter(db, "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $setWindowFields
assert.commandWorked(setParameter(
    db, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", isSbeEnabled ? 129 : 392));
// Spilling memory threshold for $bucketAuto
assert.commandWorked(setParameter(db, "internalDocumentSourceBucketAutoMaxMemoryBytes", 1));
// Spilling memory threshold for $lookup and $lookup-$unwind
assert.commandWorked(setParameter(
    db, "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $graphLookup
assert.commandWorked(setParameter(db, "internalDocumentSourceGraphLookupMaxMemoryBytes", 1));

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(foreignColl.insert({_id: i}));
}

const pipelines = {
    sort: [{$sort: {a: 1}}],
    group: [{$group: {_id: "$_id", a: {$sum: "$_id"}}}],
    setWindowFields: [{
        $setWindowFields: {
            partitionBy: "$_id",
            sortBy: {_id: 1},
            output: {b: {$sum: "$_id", window: {documents: [0, 0]}}}
        }
    }],
    lookup: [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}}],
    "lookup-unwind": [{
        $lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}
    },
    {$unwind: "$c"}],
    bucketAuto: [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {count: {$sum: 1}}}}],
    graphLookup: [{
        $graphLookup: {
            from: foreignCollName,
            startWith: "$_id",
            connectToField: "_id",
            connectFromField: "_id",
            as: "c",
        }
    }],
};

function getServerStatusSpillingMetrics(serverStatus, stageName, getLegacy) {
    if (stageName === 'sort') {
        const sortMetrics = serverStatus.metrics.query.sort;
        return {
            spills: sortMetrics.spillToDisk,
            spilledBytes: sortMetrics.spillToDiskBytes,
        };
    } else if (stageName === 'group') {
        const group = serverStatus.metrics.query.group;
        return {
            spills: group.spills,
            spilledBytes: group.spilledBytes,
        };
    } else if (stageName === 'setWindowFields') {
        const setWindowFieldsMetrics = serverStatus.metrics.query.setWindowFields;
        return {
            spills: setWindowFieldsMetrics.spills,
            spilledBytes: setWindowFieldsMetrics.spilledBytes,
        };
    } else if (stageName === 'bucketAuto') {
        const bucketAutoMetrics = serverStatus.metrics.query.bucketAuto;
        return {
            spills: bucketAutoMetrics.spills,
            spilledBytes: bucketAutoMetrics.spilledBytes,
        };
    } else if (stageName === 'lookup' || stageName === 'lookup-unwind') {
        const lookupMetrics = serverStatus.metrics.query.lookup;
        if (getLegacy === true) {
            return {
                spills: lookupMetrics.hashLookupSpillToDisk,
                spilledBytes: lookupMetrics.hashLookupSpillToDiskBytes,
            };
        } else {
            return {
                spills: lookupMetrics.hashLookupSpills,
                spilledBytes: lookupMetrics.hashLookupSpilledBytes,
            };
        }
    } else if (stageName === "graphLookup") {
        const graphLookupMetrics = serverStatus.metrics.query.graphLookup;
        return {
            spills: graphLookupMetrics.spills,
            spilledBytes: graphLookupMetrics.spilledBytes,
        };
    } else {
        return {
            spills: 0,
            spilledBytes: 0,
        };
    }
}

function testSpillingMetrics(
    {stageName, expectedSpillingMetrics, expectedSbeSpillingMetrics, getLegacy = false}) {
    const pipeline = pipelines[stageName];

    // Check whether the aggregation uses SBE.
    const explain = db[collName].explain().aggregate(pipeline);
    jsTestLog(explain);
    const queryPlanner = getQueryPlanner(explain);
    const isSbe = queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan");
    const isCollScan = getPlanStages(getWinningPlanFromExplain(explain), "COLLSCAN").length > 0;

    // Collect the serverStatus metrics before the aggregation runs.
    jsTestLog(stageName);
    const spillingMetrics = [];
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Run an aggregation and hang at the fail point in the middle of the processing.
    const failPointName =
        isSbe ? "hangScanGetNext" : (isCollScan ? "hangCollScanDoWork" : "hangFetchDoWork");
    const failPoint = configureFailPoint(db, failPointName, {} /* data */, {"skip": nDocs / 2});
    const awaitShell = startParallelShell(funWithArgs((pipeline, collName) => {
                                              const results =
                                                  db[collName].aggregate(pipeline).toArray();
                                              assert.gt(results.length, 0, results);
                                          }, pipeline, collName), conn.port);

    // Collect the serverStatus metrics once the aggregation hits the fail point.
    failPoint.wait();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Turn off the fail point and collect the serverStatus metrics after the aggregation finished.
    failPoint.off();
    awaitShell();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Assert spilling metrics are updated during the aggregation.
    for (let prop of ['spills', 'spilledBytes']) {
        const metrics = spillingMetrics.map(x => x[prop]);
        // We should have three non-decreasing metrics.
        assert.lte(metrics[0], metrics[1], spillingMetrics);
        assert.lte(metrics[1], metrics[2], spillingMetrics);
    }

    // Assert the final spilling metrics are as expected.
    assert.docEq(isSbe ? expectedSbeSpillingMetrics : expectedSpillingMetrics,
                 spillingMetrics[2],
                 spillingMetrics);
}

testSpillingMetrics({
    stageName: 'sort',
    expectedSpillingMetrics: {spills: 19, spilledBytes: 654},
    expectedSbeSpillingMetrics: {spills: 19, spilledBytes: 935}
});
testSpillingMetrics({
    stageName: 'group',
    expectedSpillingMetrics: {spills: 10, spilledBytes: 1920},
    expectedSbeSpillingMetrics: {spills: 10, spilledBytes: 450}
});
testSpillingMetrics({
    stageName: 'setWindowFields',
    expectedSpillingMetrics: {spills: 10, spilledBytes: 500},
    expectedSbeSpillingMetrics: {spills: 9, spilledBytes: 500},
});
if (isSbeEnabled) {
    // Each new aggregations increases the 'lookup' metrics by
    // 20 spills and 471 spilledByes.
    testSpillingMetrics({
        stageName: 'lookup',
        expectedSbeSpillingMetrics: {spills: 20, spilledBytes: 471},
        getLegacy: true,
    });
    testSpillingMetrics({
        stageName: 'lookup',
        expectedSbeSpillingMetrics: {spills: 40, spilledBytes: 942},
        getLegacy: false,
    });
    testSpillingMetrics({
        stageName: 'lookup-unwind',
        expectedSbeSpillingMetrics: {spills: 60, spilledBytes: 1413},
        getLegacy: true,
    });
    testSpillingMetrics({
        stageName: 'lookup-unwind',
        expectedSbeSpillingMetrics: {spills: 80, spilledBytes: 1884},
        getLegacy: false,
    });
}
testSpillingMetrics({
    stageName: 'bucketAuto',
    expectedSpillingMetrics: {spills: 19, spilledBytes: 4224},
    expectedSbeSpillingMetrics: {spills: 19, spilledBytes: 4224},
});
testSpillingMetrics({
    stageName: 'graphLookup',
    expectedSpillingMetrics: {spills: 30, spilledBytes: 1460},
    expectedSbeSpillingMetrics: {spills: 30, spilledBytes: 1460},
});

/*
 * Tests that query fails when attempting to spill with insufficient disk space
 */
function testSpillingQueryFailsWithLowDiskSpace(pipeline) {
    const simulateAvailableDiskSpaceFp =
        configureFailPoint(db, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

    // Query should fail with OutOfDiskSpace error
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}),
        ErrorCodes.OutOfDiskSpace);

    simulateAvailableDiskSpaceFp.off();
}

testSpillingQueryFailsWithLowDiskSpace(pipelines['sort']);
testSpillingQueryFailsWithLowDiskSpace(pipelines['group']);
testSpillingQueryFailsWithLowDiskSpace(pipelines['setWindowFields']);
if (isSbeEnabled) {
    testSpillingQueryFailsWithLowDiskSpace(pipelines['lookup']);
    testSpillingQueryFailsWithLowDiskSpace(pipelines['lookup-unwind']);
}

// Simulate available disk space to be more than `internalQuerySpillingMinAvailableDiskSpaceBytes`
// but less than the minimum space requirement during `mergeSpills` phase of sorter so that it fails
// with OutOfDiskSpace error during merge spills of the sort query.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySpillingMinAvailableDiskSpaceBytes: 10}));
const simulateAvailableDiskSpaceFp =
    configureFailPoint(db, 'simulateAvailableDiskSpace', {bytes: 20});

assert.commandFailedWithCode(
    db.runCommand({aggregate: collName, pipeline: pipelines['sort'], cursor: {}}),
    ErrorCodes.OutOfDiskSpace);

MongoRunner.stopMongod(conn);
