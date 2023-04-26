/**
 * Tests that when $out/$merge is run on a secondary and the primary steps down, the command
 * will fail with a `NotWritablePrimary` error.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // For configureFailPoint(), wait(), and off().

let replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
const nDocs = 100;
const dbName = jsTestName();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let primaryDB = primary.getDB(dbName);
secondary.setReadPref("secondary");

const inputCollName = "inputColl";
const outputCollName = "outputColl";
const inputCollPrimary = primaryDB.getCollection(inputCollName);

for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(inputCollPrimary.insert({_id: i, a: i + 1}, {writeConcern: {w: 2}}));
}

replTest.awaitReplication();

/**
 * Given an agg 'writeStage' (an $out or $merge), passed as a string, enables and waits for
 * 'failpoint' to be reached by the aggregate containing 'writeStage' running on a secondary and
 * verifies that the aggregate fails with a 'NotWritablePrimary' error when the primary is forced to
 * step down.
 */
let runTest = function(writeStage, failpoint) {
    let outFn = `
    const sourceDB = db.getSiblingDB("${dbName}");
    let cmdRes = sourceDB.runCommand({
        aggregate: "${inputCollName}",
        pipeline: [
        {$addFields: {b: {$sum: ['$a', '$_id']}}},
         ${writeStage}],
        cursor: {},
        $readPreference: {mode: "secondary"}
    });
    
    assert.commandFailed(cmdRes);
    assert(ErrorCodes.isNotPrimaryError(cmdRes.code), cmdRes);
    `;

    // Enable the fail point to stop the aggregate.
    const failPoint = configureFailPoint(secondary, failpoint);

    // Issue aggregate against the secondary.
    let aggOnSecondary = startParallelShell(outFn, secondary.port);
    // Wait for the aggregate to hit the fail point.
    failPoint.wait();

    // Force current primary to step down.
    assert.commandWorked(primaryDB.adminCommand({"replSetStepDown": 60 * 60, "force": true}));

    failPoint.off();

    // Join the aggregate.
    aggOnSecondary();
};

const mergeFailPoint = "hangWhileBuildingDocumentSourceMergeBatch";
const outFailPoint = "outWaitAfterTempCollectionCreation";

const mergeStage = `{$merge: {
into: "${outputCollName}",
whenMatched: "fail",
whenNotMatched: "insert",
}}`;

runTest(mergeStage, mergeFailPoint);

// Wait for the replica set to select a primary before running the next aggregate command.
replTest.awaitNodesAgreeOnPrimary();
primary = replTest.getPrimary();
primaryDB = primary.getDB(dbName);
secondary = replTest.getSecondary();

const outStage = `{$out: "${outputCollName}"}`;
runTest(outStage, outFailPoint);

replTest.stopSet();
})();
