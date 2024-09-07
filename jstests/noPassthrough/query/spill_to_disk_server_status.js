// Tests that serverStatus() updates spilling statistics while the query is running instead of after
// the query finishes.
//
// @tags: [
//   requires_persistence,
// ]
import {getPlanStages, getQueryPlanner, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

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
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalDocumentSourceCursorInitialBatchSize: 1}));
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 1}));
// Spilling memory threshold for $sort
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}));
// Spilling memory threshold for $group
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 1}));
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1}));
// Spilling memory threshold for $setWindowFields
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: isSbeEnabled ? 129 : 232
}));
// Spilling memory threshold for $lookup
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
}));

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(foreignColl.insert({_id: i}));
}

function getServerStatusSpillingMetrics(serverStatus, stageName) {
    if (stageName === '$sort') {
        const sortMetrics = serverStatus.metrics.query.sort;
        return {
            spills: sortMetrics.spillToDisk,
            spilledBytes: sortMetrics.spillToDiskBytes,
        };
    } else if (stageName === '$group') {
        const group = serverStatus.metrics.query.group;
        return {
            spills: group.spills,
            spilledBytes: group.spilledBytes,
        };
    } else if (stageName === '$setWindowFields') {
        const setWindowFieldsMetrics = serverStatus.metrics.query.setWindowFields;
        return {
            spills: setWindowFieldsMetrics.spills,
            spilledBytes: setWindowFieldsMetrics.spilledBytes,
        };
    } else if (stageName === '$lookup') {
        const lookupMetrics = serverStatus.metrics.query.lookup;
        return {
            spills: lookupMetrics.hashLookupSpillToDisk,
            spilledBytes: lookupMetrics.hashLookupSpillToDiskBytes,
        };
    }
    return {
        spills: 0,
        spilledBytes: 0,
    };
}

function testSpillingMetrics({stage, expectedSpillingMetrics, expectedSbeSpillingMetrics}) {
    // Check whether the aggregation uses SBE.
    const explain = db[collName].explain().aggregate([stage]);
    jsTestLog(explain);
    const queryPlanner = getQueryPlanner(explain);
    const isSbe = queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan");
    const isCollScan = getPlanStages(getWinningPlan(queryPlanner), "COLLSCAN").length > 0;

    // Collect the serverStatus metrics before the aggregation runs.
    const stageName = Object.keys(stage)[0];
    const spillingMetrics = [];
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName));

    // Run an aggregation and hang at the fail point in the middle of the processing.
    const failPointName =
        isSbe ? "hangScanGetNext" : (isCollScan ? "hangCollScanDoWork" : "hangFetchDoWork");
    const failPoint = configureFailPoint(db, failPointName, {} /* data */, {"skip": nDocs / 2});
    const awaitShell = startParallelShell(funWithArgs((stage, collName) => {
                                              const results =
                                                  db[collName].aggregate([stage]).toArray();
                                              assert.gt(results.length, 0, results);
                                          }, stage, collName), conn.port);

    // Collect the serverStatus metrics once the aggregation hits the fail point.
    failPoint.wait();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName));

    // Turn off the fail point and collect the serverStatus metrics after the aggregation finished.
    failPoint.off();
    awaitShell();
    spillingMetrics.push(getServerStatusSpillingMetrics(db.serverStatus(), stageName));

    // Assert spilling metrics are updated during the aggregation.
    for (let prop of ['spills', 'spilledBytes']) {
        const metrics = spillingMetrics.map(x => x[prop]);
        // We should have three increasing metrics.
        assert.lt(metrics[0], metrics[1], spillingMetrics);
        assert.lt(metrics[1], metrics[2], spillingMetrics);
    }

    // Assert the final spilling metrics are as expected.
    assert.docEq(isSbe ? expectedSbeSpillingMetrics : expectedSpillingMetrics,
                 spillingMetrics[2],
                 spillingMetrics);
}

testSpillingMetrics({
    stage: {$sort: {a: 1}},
    expectedSpillingMetrics: {spills: 19, spilledBytes: 654},
    expectedSbeSpillingMetrics: {spills: 19, spilledBytes: 935}
});
testSpillingMetrics({
    stage: {$group: {_id: "$_id", a: {$sum: "$_id"}}},
    expectedSpillingMetrics: {spills: 10, spilledBytes: 410},
    expectedSbeSpillingMetrics: {spills: 10, spilledBytes: 450}
});
testSpillingMetrics({
    stage: {
        $setWindowFields: {
            partitionBy: "$_id",
            sortBy: {_id: 1},
            output: {b: {$sum: "$_id", window: {documents: [0, 0]}}}
        }
    },
    expectedSpillingMetrics: {spills: 10, spilledBytes: 180},
    expectedSbeSpillingMetrics: {spills: 9, spilledBytes: 500},
});
if (isSbeEnabled) {
    testSpillingMetrics({
        stage: {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}},
        expectedSbeSpillingMetrics: {spills: 20, spilledBytes: 790},
    });
}

MongoRunner.stopMongod(conn);
