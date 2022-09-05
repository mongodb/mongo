/**
 * Test that CollectionCloner completes without error when a collection is renamed during cloning.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load('jstests/replsets/libs/two_phase_drops.js');

// Set up replica set. Disallow chaining so nodes always sync from primary.
const testName = "initial_sync_rename_collection";
const dbName = testName;
const replTest = new ReplSetTest(
    {name: testName, nodes: [{}, {rsConfig: {priority: 0}}], settings: {chainingAllowed: false}});
replTest.startSet();
replTest.initiate();

const collName = "testcoll";
const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const pRenameColl = primaryDB["r_" + collName];

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Used for cross-DB renames.
const secondDbName = testName + "_cross";
const primarySecondDB = primary.getDB(secondDbName);
const pCrossDBRenameColl = primarySecondDB[collName + "_cross"];

const nss = primaryColl.getFullName();
const rnss = pRenameColl.getFullName();
let secondary = replTest.getSecondary();
let secondaryDB = secondary.getDB(dbName);
let secondaryColl = secondaryDB[collName];

// This function adds data to the collection, restarts the secondary node with the given
// parameters and setting the given failpoint, waits for the failpoint to be hit,
// renames the collection, then disables the failpoint.  It then optionally waits for the
// expectedLog message and waits for the secondary to complete initial sync, then ensures
// the collection on the secondary has been properly cloned.
function setupTest({failPoint, extraFailPointData, secondaryStartupParams}) {
    jsTestLog("Writing data to collection.");
    assert.commandWorked(primaryColl.insert([{_id: 1}, {_id: 2}]));
    const data = Object.merge(extraFailPointData || {}, {nss: nss});

    jsTestLog("Restarting secondary with failPoint " + failPoint + " set for " + nss);
    secondaryStartupParams = secondaryStartupParams || {};
    secondaryStartupParams['failpoint.' + failPoint] = tojson({mode: 'alwaysOn', data: data});
    // Skip clearing initial sync progress after a successful initial sync attempt so that we
    // can check initialSyncStatus fields after initial sync is complete.
    secondaryStartupParams['failpoint.skipClearInitialSyncState'] = tojson({mode: 'alwaysOn'});
    secondaryStartupParams['numInitialSyncAttempts'] = 1;
    secondary =
        replTest.restart(secondary, {startClean: true, setParameter: secondaryStartupParams});
    secondaryDB = secondary.getDB(dbName);
    secondaryColl = secondaryDB[collName];

    jsTestLog("Waiting for secondary to reach failPoint " + failPoint);
    assert.commandWorked(secondary.adminCommand({
        waitForFailPoint: failPoint,
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Restarting the secondary may have resulted in an election.  Wait until the system
    // stabilizes and reaches RS_STARTUP2 state.
    replTest.getPrimary();
    replTest.waitForState(secondary, ReplSetTest.State.STARTUP_2);
}

function finishTest({failPoint, expectedLog, createNew, renameAcrossDBs}) {
    // Get the uuid for use in checking the log line.
    const uuid_obj = getUUIDFromListCollections(primaryDB, collName);
    const uuid = extractUUIDFromObject(uuid_obj);
    const target = (renameAcrossDBs ? pCrossDBRenameColl : pRenameColl);

    jsTestLog("Renaming collection on primary: " + target.getFullName());
    assert.commandWorked(primary.adminCommand({
        renameCollection: primaryColl.getFullName(),
        to: target.getFullName(),
        dropTarget: false
    }));

    // Only set for test cases that use 'system.drop' namespaces when dropping collections.
    // In those tests the variable 'dropPendingNss' represents such a namespace. Used for
    // expectedLog. See test cases 6 and 8 below.
    let dropPendingNss;
    const dropPendingColl =
        TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(primaryDB, collName);
    if (dropPendingColl) {
        dropPendingNss = dbName + "." + dropPendingColl["name"];
    }

    if (createNew) {
        jsTestLog("Creating a new collection with the same name: " + primaryColl.getFullName());
        assert.commandWorked(primaryColl.insert({_id: "not the same collection"}));
    }

    jsTestLog("Allowing secondary to continue.");
    assert.commandWorked(secondary.adminCommand({configureFailPoint: failPoint, mode: 'off'}));

    if (expectedLog) {
        expectedLog = eval(expectedLog);
        jsTestLog(expectedLog);
        checkLog.contains(secondary, expectedLog);
    }

    jsTestLog("Waiting for initial sync to complete.");
    replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

    let res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);

    if (createNew) {
        assert.eq([{_id: "not the same collection"}], secondaryColl.find().toArray());
        assert(primaryColl.drop());
    } else {
        assert.eq(0, secondaryColl.find().itcount());
    }
    replTest.checkReplicatedDataHashes();

    // Drop the renamed collection so we can start fresh the next time around.
    assert(target.drop());
}

function runRenameTest(params) {
    setupTest(params);
    finishTest(params);
}

jsTestLog("[1] Testing rename between listIndexes and find.");
runRenameTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"}
});

jsTestLog("[2] Testing cross-DB rename between listIndexes and find.");
runRenameTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"},
    renameAcrossDBs: true
});

jsTestLog(
    "[3] Testing rename between listIndexes and find, with new same-name collection created.");
runRenameTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"},
    createNew: true
});

jsTestLog(
    "[4] Testing cross-DB rename between listIndexes and find, with new same-name collection created.");
runRenameTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"},
    createNew: true,
    renameAcrossDBs: true
});

const expectedLogFor5and7 =
    '`Sync process retrying cloner stage due to error","attr":{"cloner":"CollectionCloner","stage":"query","error":{"code":175,"codeName":"QueryPlanKilled","errmsg":"collection renamed from \'${nss}\' to \'${rnss}\'. UUID ${uuid}"}}}`';

jsTestLog("[5] Testing rename between getMores.");
runRenameTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: expectedLogFor5and7
});

// A cross-DB rename will appear as a drop in the context of the source DB.
// Double escape the backslash as eval will do unescaping
const expectedLogFor6and8 =
    '`CollectionCloner stopped because collection was dropped on source","attr":{"namespace":"${nss}","uuid":{"uuid":{"$uuid":"${uuid}"}}}}`';

// We don't support 4.2 style two-phase drops with EMRC=false - in that configuration, the
// collection will instead be renamed to a <db>.system.drop.* namespace before being dropped. Since
// the cloner queries collection by UUID, it will observe the first drop phase as a rename.
// We still want to check that initial sync succeeds in such a case.
if (TwoPhaseDropCollectionTest.supportsDropPendingNamespaces(replTest)) {
    expectedLogFor6and8 =
        '`Sync process retrying cloner stage due to error","attr":{"cloner":"CollectionCloner","stage":"query","error":{"code":175,"codeName":"QueryPlanKilled","errmsg":"collection renamed from \'${nss}\' to \'${dropPendingNss}\'. UUID ${uuid}`';
}

jsTestLog("[6] Testing cross-DB rename between getMores.");
runRenameTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    renameAcrossDBs: true,
    expectedLog: expectedLogFor6and8
});

jsTestLog("[7] Testing rename with new same-name collection created, between getMores.");
runRenameTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: expectedLogFor5and7
});

jsTestLog("[8] Testing cross-DB rename with new same-name collection created, between getMores.");
runRenameTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    renameAcrossDBs: true,
    expectedLog: expectedLogFor6and8
});

replTest.stopSet();
})();
