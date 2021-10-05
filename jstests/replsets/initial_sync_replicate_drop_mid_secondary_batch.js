/**
 * It was previously possible during initial sync for the oplog fetcher to miss a drop that the
 * cloner observed, specifically when it happens during a not-yet-finalized batch on the sync
 * source. This tests that this is no longer possible.
 *
 * @tags: [requires_replication, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const dbName = "testdb";
const collName = "testcoll";
const testNss = dbName + "." + collName;

const primary = rst.getPrimary();
const syncSource = rst.getSecondaries()[0];
let initSyncNode = rst.getSecondaries()[1];

var verbosityCmd = {
    "setParameter": 1,
    "logComponentVerbosity": {
        "replication": {"verbosity": 3},
    }
};
assert.commandWorked(syncSource.adminCommand(verbosityCmd));

jsTestLog("Inserting some docs on the primary");
const primaryDB = primary.getDB(dbName);
assert.commandWorked(primaryDB.getCollection(collName).insert({"a": 1}));
assert.commandWorked(primaryDB.getCollection(collName).insert({"b": 2}));
rst.awaitReplication();

const clonerFailPoint = "hangBeforeClonerStage";
const failPointData = {
    cloner: "CollectionCloner",
    stage: "listIndexes",
    nss: testNss
};

jsTestLog("Restarting last node for initial sync");
let startupParams = {};
startupParams["logComponentVerbosity"] = tojson({replication: 3});
startupParams["initialSyncSourceReadPreference"] = "secondaryPreferred";
startupParams["failpoint." + clonerFailPoint] = tojson({mode: "alwaysOn", data: failPointData});
initSyncNode = rst.restart(initSyncNode, {startClean: true, setParameter: startupParams});

jsTestLog("Waiting for initial syncing node to hit failpoint");
assert.commandWorked(initSyncNode.adminCommand({
    waitForFailPoint: clonerFailPoint,
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Adding more data to initial sync");
assert.commandWorked(primaryDB.getCollection(collName).insert({"c": 3}));
rst.awaitReplication(undefined /* timeout */, undefined /*secondaryOpTimeType */, [syncSource]);

const dropFailPoint =
    configureFailPoint(syncSource, "hangAfterApplyingCollectionDropOplogEntry", {dbName: dbName});

assert(primaryDB.getCollection(collName).drop({writeConcern: {w: 1}}));

jsTestLog("Waiting for sync source to hit drop failpoint");
dropFailPoint.wait();
sleep(10 * 1000);

// Enable this so we too can see the drop entry.
const allowExternalReadsFp =
    configureFailPoint(syncSource, "allowExternalReadsForReverseOplogScanRule");

const syncSourceEntries =
    syncSource.getCollection('local.oplog.rs').find({ns: /testdb/i}).sort({$natural: -1}).toArray();

syncSourceEntries.forEach(function(entry) {
    jsTestLog("Sync source entry: " + tojson(entry));
});

const latestSyncSourceEntry = syncSourceEntries[0];

assert(latestSyncSourceEntry.o, () => tojson(latestSyncSourceEntry));
assert(latestSyncSourceEntry.o.drop, () => tojson(latestSyncSourceEntry));
assert.eq(collName, latestSyncSourceEntry.o.drop, () => tojson(latestSyncSourceEntry));

const targetStopTs = latestSyncSourceEntry.ts;

allowExternalReadsFp.off();

jsTestLog("Resuming initial sync");
assert.commandWorked(initSyncNode.adminCommand({configureFailPoint: clonerFailPoint, mode: 'off'}));

jsTestLog("Waiting for initial sync node to reach correct stopTimestamp");
assert.soonNoExcept(function() {
    const nodeStatus = assert.commandWorked(initSyncNode.adminCommand({replSetGetStatus: 1}));
    assert(nodeStatus, () => tojson(nodeStatus));
    assert(nodeStatus.initialSyncStatus, () => tojson(nodeStatus));
    // Is actually the 'stopTimestamp'.
    assert(nodeStatus.initialSyncStatus.initialSyncOplogEnd, () => tojson(nodeStatus));
    const currentStopTs = nodeStatus.initialSyncStatus.initialSyncOplogEnd;
    assert.eq(currentStopTs, targetStopTs, () => tojson(nodeStatus));

    const comparison = bsonWoCompare(currentStopTs, targetStopTs);

    if (comparison == 0) {
        // We reached the stopTs we wanted.
        return true;
    }

    // We should never not exceed that timestamp.
    assert.lte(currentStopTs, targetStopTs, () => tojson(nodeStatus));
    return false;
});

// Now that the stopTimestamp is far enough to include the drop, we also need to allow
// the fetcher to actually replicate those entries.
jsTestLog("Resuming batch application on the secondary");
dropFailPoint.off();

jsTestLog("Waiting for initial sync to complete");
rst.waitForState(initSyncNode, ReplSetTest.State.SECONDARY);  // will time out on error

rst.awaitReplication();
rst.stopSet();
})();
