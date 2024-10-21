/**
 * Test that CollectionCloner completes without error when a collection is dropped during cloning.
 */

import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

// Set up replica set. Disallow chaining so nodes always sync from primary.
const testName = "initial_sync_drop_collection";
const dbName = testName;
var replTest = new ReplSetTest(
    {name: testName, nodes: [{}, {rsConfig: {priority: 0}}], settings: {chainingAllowed: false}});
replTest.startSet();
replTest.initiate();

var primary = replTest.getPrimary();
var primaryDB = primary.getDB(dbName);
var secondary = replTest.getSecondary();
var secondaryDB = secondary.getDB(dbName);
const collName = "testcoll";
var primaryColl = primaryDB[collName];
var secondaryColl = secondaryDB[collName];
var nss = primaryColl.getFullName();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

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
    secondaryStartupParams = Object.merge(
        secondaryStartupParams, {logComponentVerbosity: tojson({replication: {verbosity: 2}})});
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

function finishTest({failPoint, expectedLog, createNew}) {
    // Get the uuid for use in checking the log line.
    const uuid_obj = getUUIDFromListCollections(primaryDB, collName);
    const uuid = extractUUIDFromObject(uuid_obj);

    jsTestLog("Doing further inserts and updates on the collection");
    assert.commandWorked(primaryColl.insert([{_id: 11}]));
    assert.commandWorked(primaryColl.update({_id: 1}, {a: 2}));
    assert.commandWorked(primaryColl.update({_id: 11}, {a: 22}));
    assert.commandWorked(primaryColl.remove({_id: 2}));
    jsTestLog("Dropping collection on primary: " + primaryColl.getFullName());
    assert(primaryColl.drop());

    if (createNew) {
        jsTestLog("Creating a new collection with the same name: " + primaryColl.getFullName());
        assert.commandWorked(primaryColl.insert({_id: "not the same collection"}));
    }

    jsTestLog("Allowing secondary to continue.");
    assert.commandWorked(secondary.adminCommand({configureFailPoint: failPoint, mode: 'off'}));

    if (expectedLog) {
        let expectedLogDict = eval('(' + expectedLog + ')');
        jsTestLog(expectedLogDict);
        checkLog.containsJson(secondary, expectedLogDict.code, expectedLogDict.attr);
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

    // The additional ops should fail with NamespaceNotFound.  We ignore the failure but
    // log it.
    const kNamespaceNotFoundInCrudOp = 9067401;
    checkLog.checkContainsWithCountJson(
        secondary,
        kNamespaceNotFoundInCrudOp,
        {} /*attrs*/,
        4 /* count */,
        null /* severity */,
        true /* isRelaxed */,
        (actual, expected) => {
            assert.eq(actual, expected, "Wrong number of NamespaceNotFound log messages");
            return true;
        });
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

let expectedLogFor3and4 =
    '{code: 21132, attr: { namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid)}}';

jsTestLog("[3] Testing drop-pending between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: expectedLogFor3and4
});

jsTestLog("[4] Testing drop-pending with new same-name collection created, between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: expectedLogFor3and4,
    createNew: true
});

// Add another node to the set, so when we drop the collection it can commit.  This other
// secondary will be finished with initial sync when the drop happens.
var secondary2 = replTest.add({rsConfig: {priority: 0}});
replTest.reInitiate();
replTest.waitForState(secondary2, ReplSetTest.State.SECONDARY);

jsTestLog("[5] Testing committed drop between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: '{code: 21132, attr:{namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid)}}',
});

jsTestLog(
    "[6] Testing committed drop with new same-name collection created, between getMore calls.");
runDropTest({
    failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    secondaryStartupParams: {collectionClonerBatchSize: 1},
    expectedLog: '{code: 21132, attr:{namespace: nss, uuid: (x)=>(x.uuid.$uuid === uuid)}}',
    createNew: true
});

replTest.stopSet();
