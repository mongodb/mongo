// Tests that serverStatus() updates spilling statistics while the query is running instead of after
// the query finishes.
//
// @tags: [
//   requires_persistence,
// ]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getEngine, getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
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
const geoCollName = "spill_to_disk_server_status_geo";
const geoColl = db[geoCollName];
geoColl.drop();
const isSbeEnabled = checkSbeFullyEnabled(db);

// Set up relevant query knobs so that the query will spill for every document.
// No batching in document source cursor
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorInitialBatchSize", 1));
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorBatchSizeBytes", 1));
// Spilling memory threshold for $sort
assert.commandWorked(setParameter(db, "internalQueryMaxBlockingSortMemoryUsageBytes", 1));
// Spilling memory threshold for $group
assert.commandWorked(setParameter(db, "internalDocumentSourceGroupMaxMemoryBytes", 1));
assert.commandWorked(setParameter(db, "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $setWindowFields
assert.commandWorked(setParameter(db, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", isSbeEnabled ? 129 : 424));
// Spilling memory threshold for $bucketAuto
assert.commandWorked(setParameter(db, "internalDocumentSourceBucketAutoMaxMemoryBytes", 1));
// Spilling memory threshold for $lookup and $lookup-$unwind
assert.commandWorked(setParameter(db, "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $graphLookup
assert.commandWorked(setParameter(db, "internalDocumentSourceGraphLookupMaxMemoryBytes", 1));
// Spilling memory threshold for geo near
assert.commandWorked(setParameter(db, "internalNearStageMaxMemoryBytes", 1));

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(foreignColl.insert({_id: i}));
    assert.commandWorked(geoColl.insert({_id: i, geo: [(Math.cos(i) * i) / 10, (Math.sin(i) * i) / 10]}));
}

assert.commandWorked(geoColl.createIndex({geo: "2d"}));

const pipelines = {
    sort: [{$sort: {a: 1}}],
    group: [{$group: {_id: "$_id", a: {$sum: "$_id"}}}],
    setWindowFields: [
        {
            $setWindowFields: {
                partitionBy: "$_id",
                sortBy: {_id: 1},
                output: {b: {$sum: "$_id", window: {documents: [0, 0]}}},
            },
        },
    ],
    lookup: [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}}],
    "lookup-unwind": [
        {
            $lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"},
        },
        {$unwind: "$c"},
    ],
    bucketAuto: [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {count: {$sum: 1}}}}],
    graphLookup: [
        {
            $graphLookup: {
                from: foreignCollName,
                startWith: "$_id",
                connectToField: "_id",
                connectFromField: "_id",
                as: "c",
            },
        },
    ],
    geoNear: [{$geoNear: {near: [0, 0]}}],
};

function getServerStatusSpillingMetrics(serverStatus, stageName, getLegacy) {
    if (stageName === "sort") {
        const sortMetrics = serverStatus.metrics.query.sort;
        return {
            spills: sortMetrics.spillToDisk,
            spilledBytes: sortMetrics.spillToDiskBytes,
        };
    } else if (stageName === "group") {
        const group = serverStatus.metrics.query.group;
        return {
            spills: group.spills,
            spilledBytes: group.spilledBytes,
        };
    } else if (stageName === "setWindowFields") {
        const setWindowFieldsMetrics = serverStatus.metrics.query.setWindowFields;
        return {
            spills: setWindowFieldsMetrics.spills,
            spilledBytes: setWindowFieldsMetrics.spilledBytes,
        };
    } else if (stageName === "bucketAuto") {
        const bucketAutoMetrics = serverStatus.metrics.query.bucketAuto;
        return {
            spills: bucketAutoMetrics.spills,
            spilledBytes: bucketAutoMetrics.spilledBytes,
        };
    } else if (stageName === "lookup" || stageName === "lookup-unwind") {
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
    } else if (stageName === "geoNear") {
        const geoNearMetrics = serverStatus.metrics.query.geoNear;
        return {
            spills: geoNearMetrics.spills,
            spilledBytes: geoNearMetrics.spilledBytes,
        };
    } else {
        return {
            spills: 0,
            spilledBytes: 0,
        };
    }
}

function getClassicFailpointName(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    return isCollscan(db, winningPlan) ? "hangCollScanDoWork" : "hangFetchDoWork";
}

function getSbeFailpointName(explain) {
    const slotBasedPlan = getWinningPlanFromExplain(explain, true /* isSbePlan */);
    const isScan = slotBasedPlan.stages.includes("scan");
    if (isScan) {
        const isGenericScan = slotBasedPlan.stages.includes("scan generic");
        return isGenericScan ? "hangGenericScanGetNext" : "hangScanGetNext";
    }
    return "hangFetchGetNext";
}

function testSpillingMetrics({
    stageName,
    expectedSpillingMetrics,
    expectedSbeSpillingMetrics,
    getLegacy = false,
    collName = coll.getName(),
}) {
    const pipeline = pipelines[stageName];

    // Check whether the aggregation uses SBE.
    const explain = db[collName].explain().aggregate(pipeline);
    const isSbePlan = getEngine(explain) === "sbe";

    // Collect the serverStatus metrics before the aggregation runs.
    const spillingMetrics = [];
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Run an aggregation and hang at the fail point in the middle of the processing.
    const failPointName = isSbePlan ? getSbeFailpointName(explain) : getClassicFailpointName(explain);
    const failPoint = configureFailPoint(db, failPointName, {} /* data */, {"skip": nDocs / 2});
    const awaitShell = startParallelShell(
        funWithArgs(
            (pipeline, collName) => {
                const results = db[collName].aggregate(pipeline).toArray();
                assert.gt(results.length, 0, results);
            },
            pipeline,
            collName,
        ),
        conn.port,
    );

    // Collect the serverStatus metrics once the aggregation hits the fail point.
    failPoint.wait();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Turn off the fail point and collect the serverStatus metrics after the aggregation finished.
    failPoint.off();
    awaitShell();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName, getLegacy));

    // Assert spilling metrics are updated during the aggregation.
    for (let prop of ["spills", "spilledBytes"]) {
        const metrics = spillingMetrics.map((x) => x[prop]);
        // We should have three non-decreasing metrics.
        assert.lte(metrics[0], metrics[1], spillingMetrics);
        assert.lte(metrics[1], metrics[2], spillingMetrics);
    }

    // Assert the final spilling metrics are as expected.
    const expected = isSbePlan ? expectedSbeSpillingMetrics : expectedSpillingMetrics;
    const actual = spillingMetrics[2];
    assert.docEq(
        expected,
        actual,
        `Expected ${expected} but found ${actual}. spillingMetrics=${tojson(spillingMetrics)}`,
    );
}

testSpillingMetrics({
    stageName: "sort",
    expectedSpillingMetrics: {spills: 19, spilledBytes: 654},
    expectedSbeSpillingMetrics: {spills: 19, spilledBytes: 935},
});
testSpillingMetrics({
    stageName: "group",
    expectedSpillingMetrics: {spills: 10, spilledBytes: 2080},
    expectedSbeSpillingMetrics: {spills: 10, spilledBytes: 450},
});
testSpillingMetrics({
    stageName: "setWindowFields",
    expectedSpillingMetrics: {spills: 10, spilledBytes: 500},
    expectedSbeSpillingMetrics: {spills: 9, spilledBytes: 500},
});
if (isSbeEnabled) {
    // Each new aggregations increases the 'lookup' metrics by
    // 20 spills and 471 spilledByes.
    testSpillingMetrics({
        stageName: "lookup",
        expectedSbeSpillingMetrics: {spills: 20, spilledBytes: 471},
        getLegacy: true,
    });
    testSpillingMetrics({
        stageName: "lookup",
        expectedSbeSpillingMetrics: {spills: 40, spilledBytes: 942},
        getLegacy: false,
    });
    testSpillingMetrics({
        stageName: "lookup-unwind",
        expectedSbeSpillingMetrics: {spills: 60, spilledBytes: 1413},
        getLegacy: true,
    });
    testSpillingMetrics({
        stageName: "lookup-unwind",
        expectedSbeSpillingMetrics: {spills: 80, spilledBytes: 1884},
        getLegacy: false,
    });
}
testSpillingMetrics({
    stageName: "bucketAuto",
    expectedSpillingMetrics: {spills: 19, spilledBytes: 4224},
    expectedSbeSpillingMetrics: {spills: 19, spilledBytes: 4224},
});
testSpillingMetrics({
    stageName: "graphLookup",
    expectedSpillingMetrics: {spills: 30, spilledBytes: 1460},
    expectedSbeSpillingMetrics: {spills: 30, spilledBytes: 1460},
});
if (FeatureFlagUtil.isPresentAndEnabled(db, "ExtendedAutoSpilling")) {
    testSpillingMetrics({
        stageName: "geoNear",
        expectedSpillingMetrics: {spills: 12, spilledBytes: 810},
        expectedSbeSpillingMetrics: {spills: 20, spilledBytes: 1130},
        collName: geoCollName,
    });
}

/*
 * Tests that query fails when attempting to spill with insufficient disk space
 */
function testSpillingQueryFailsWithLowDiskSpace(pipeline) {
    const simulateAvailableDiskSpaceFp = configureFailPoint(db, "simulateAvailableDiskSpace", {
        bytes: 450 * 1024 * 1024,
    });

    // Query should fail with OutOfDiskSpace error
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}),
        ErrorCodes.OutOfDiskSpace,
    );

    simulateAvailableDiskSpaceFp.off();
}

testSpillingQueryFailsWithLowDiskSpace(pipelines["sort"]);
testSpillingQueryFailsWithLowDiskSpace(pipelines["group"]);
testSpillingQueryFailsWithLowDiskSpace(pipelines["setWindowFields"]);
if (isSbeEnabled) {
    testSpillingQueryFailsWithLowDiskSpace(pipelines["lookup"]);
    testSpillingQueryFailsWithLowDiskSpace(pipelines["lookup-unwind"]);
}

// Simulate available disk space to be more than `internalQuerySpillingMinAvailableDiskSpaceBytes`
// but less than the minimum space requirement during `mergeSpills` phase of sorter so that it fails
// with OutOfDiskSpace error during merge spills of the sort query.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySpillingMinAvailableDiskSpaceBytes: 10}));
const simulateAvailableDiskSpaceFp = configureFailPoint(db, "simulateAvailableDiskSpace", {bytes: 20});

assert.commandFailedWithCode(
    db.runCommand({aggregate: collName, pipeline: pipelines["sort"], cursor: {}}),
    ErrorCodes.OutOfDiskSpace,
);

MongoRunner.stopMongod(conn);
