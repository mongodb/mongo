/**
 * Test that CollectionCloner completes without error when a collection is dropped during cloning,
 * specifically when that sync source is in 4.2.
 */
load("jstests/libs/logv2_helpers.js");

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/replsets/libs/two_phase_drops.js');
load("jstests/libs/uuid_util.js");

// Set up replica set with two nodes. We will add a third and force it to sync from the secondary.
const testName = "initial_sync_drop_against_last_stable";
const dbName = testName;
const replTest = new ReplSetTest({
    name: testName,
    nodes: [
        {},                                                             /* primary */
        {rsConfig: {priority: 0, votes: 0}, binVersion: "last-stable"}, /* sync source */
        {rsConfig: {priority: 0, votes: 0}}                             /* initial syncing node */
    ]
});
replTest.startSet();
replTest.initiate();

const collName = "testcoll";
const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const nss = primaryColl.getFullName();

// The sync source.
const syncSource = replTest.getSecondary();

// The initial syncing node. Places in the test that refer to 'the secondary' refer to this node.
let secondary = replTest.getSecondaries()[1];
assert.neq(syncSource, secondary, "initial syncing node should be the third in the set");
let secondaryDB = secondary.getDB(dbName);
let secondaryColl = secondaryDB[collName];

// This function adds data to the collection, restarts the secondary node with the given
// parameters and setting the given failpoint, waits for the failpoint to be hit,
// drops the collection, then disables the failpoint.  It then optionally waits for the
// expectedLog message and waits for the secondary to complete initial sync, then ensures
// the collection on the secondary is empty.
function setupTest({failPoint, extraFailPointData, secondaryStartupParams}) {
    jsTestLog("Writing data to collection.");
    assert.commandWorked(primaryColl.insert([{_id: 1}, {_id: 2}]));
    const data = Object.merge(extraFailPointData || {}, {nss: nss});

    jsTestLog("Restarting secondary with failPoint " + failPoint + " set for " + nss);
    secondaryStartupParams = secondaryStartupParams || {};
    secondaryStartupParams['failpoint.' + failPoint] = tojson({mode: 'alwaysOn', data: data});
    // Force the initial syncing node to sync against the 4.2 secondary.
    secondaryStartupParams['failpoint.forceSyncSourceCandidate'] =
        tojson({mode: 'alwaysOn', data: {hostAndPort: syncSource.host}});
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

function finishTest({failPoint, expectedLog, expectedLogId, waitForDrop, createNew}) {
    // Get the uuid for use in checking the log line.
    let uuid = getUUIDFromListCollections(primaryDB, collName);

    jsTestLog("Dropping collection on primary: " + primaryColl.getFullName());
    assert(primaryColl.drop());
    replTest.awaitReplication(null, null, [syncSource]);

    if (waitForDrop) {
        jsTestLog("Waiting for drop to commit on primary");
        TwoPhaseDropCollectionTest.waitForDropToComplete(primaryDB, collName);
    }

    if (createNew) {
        jsTestLog("Creating a new collection with the same name: " + primaryColl.getFullName());
        assert.commandWorked(primaryColl.insert({_id: "not the same collection"}));
    }

    jsTestLog("Allowing secondary to continue.");
    assert.commandWorked(secondary.adminCommand({configureFailPoint: failPoint, mode: 'off'}));

    if (isJsonLog(primaryColl.getMongo())) {
        if (expectedLogId) {
            let attrValues = {
                "namespace": nss,
                "uuid": function(attr) {
                    return UUID(attr.uuid.$uuid).toString() === uuid.toString();
                }
            };

            checkLog.containsJson(secondary, expectedLogId, attrValues);
        }
    } else {
        if (expectedLog) {
            expectedLog = eval(expectedLog);
            jsTestLog(expectedLog);
            checkLog.contains(secondary, expectedLog);
        }
    }

    jsTestLog("Waiting for initial sync to complete.");
    replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);
    replTest.awaitReplication();

    let res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);

    if (createNew) {
        assert.eq([{_id: "not the same collection"}], secondaryColl.find().toArray());
        assert(primaryColl.drop());
    } else {
        assert.eq(0, secondaryColl.find().itcount());
    }

    replTest.checkReplicatedDataHashes();
}

function runDropTest(params) {
    setupTest(params);
    finishTest(params);
}

jsTestLog("[1] Testing dropping between listIndexes and find.");
runDropTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"}
});

jsTestLog(
    "[2] Testing dropping between listIndexes and find, with new same-name collection created.");
runDropTest({
    failPoint: "hangBeforeClonerStage",
    extraFailPointData: {cloner: "CollectionCloner", stage: "query"},
    createNew: true
});

jsTestLog("[3] Testing committed drop between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    waitForDrop: true,
    expectedLogId: 21132,
    expectedLog:
        "`CollectionCloner ns: '${nss}' uuid: ${uuid} stopped because collection was dropped on source.`"
});

jsTestLog(
    "[4] Testing committed drop with new same-name collection created, between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    waitForDrop: true,
    expectedLogId: 21132,
    expectedLog:
        "`CollectionCloner ns: '${nss}' uuid: ${uuid} stopped because collection was dropped on source.`",
    createNew: true
});

replTest.stopSet();
})();