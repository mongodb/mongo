/**
 * Tests that retryable findAndModify data stored in the `config.image_collection` side collection
 * do not get populated by nodes doing oplog application while in initial sync.
 *
 * This setParameter behavior does not yet exist on earlier versions.
 * @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/replsets/rslib.js');

// A secondary cannot compute retryable images during initial sync. Thus we skip db hash checks as
// its expected for config.image_collection to not match.
TestData.skipCheckDBHashes = true;

// Start a single node replica set.
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            featureFlagRetryableFindAndModify: true,
            storeFindAndModifyImagesInSideCollection: true
        }
    }
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = jsTest.name();
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB['collection'];

primaryColl.insert({_id: 1});

let lsid = UUID();
jsTestLog({
    "Pre-initial sync retryable findAndModify": assert.commandWorked(primaryDB.runCommand({
        findandmodify: primaryColl.getName(),
        lsid: {id: lsid},
        txnNumber: NumberLong(1),
        stmtId: NumberInt(1),
        query: {_id: 1},
        new: false,
        update: {$set: {preInitialSync: true}}
    }))
});

jsTestLog("Adding a new voting node (node1) to the replica set.");
const node1 = rst.add({
    rsConfig: {priority: 1, votes: 1},
    setParameter: {'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'})}
});
rst.reInitiate();

jsTestLog("Wait for node1 to hang during initial sync.");
checkLog.containsJson(node1, 21184);

// Perform a findAndModify update that will not be retryable on the node that's concurrently initial
// syncing.
let result = assert.commandWorked(primaryDB.runCommand({
    findandmodify: primaryColl.getName(),
    lsid: {id: lsid},
    txnNumber: NumberLong(2),
    stmtId: NumberInt(1),
    query: {_id: 1},
    new: false,
    update: {$set: {duringInitialSync: true}}
}));
jsTestLog({"Retryable findAndModify during initial sync": result});

// With a separate logical session, perform a findAndModify removal that will not be retryable on
// the node that's concurrently initial syncing.
let otherLsid = UUID();
jsTestLog({
    "Retryable findAndModify removal during initial sync":
        assert.commandWorked(primaryDB.runCommand({
            findandmodify: primaryColl.getName(),
            lsid: {id: otherLsid},
            txnNumber: NumberLong(3),
            stmtId: NumberInt(1),
            query: {_id: 1},
            new: false,
            remove: true
        }))
});

jsTestLog("Resuming initial sync.");
assert.commandWorked(
    node1.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: 'off'}));
rst.waitForState(node1, ReplSetTest.State.SECONDARY);

let initialSyncedNode = rst.getSecondary();
rst.stepUp(initialSyncedNode);

result = assert.commandFailedWithCode(initialSyncedNode.getDB(dbName).runCommand({
    findandmodify: primaryColl.getName(),
    lsid: {id: lsid},
    txnNumber: NumberLong(2),
    stmtId: NumberInt(1),
    query: {_id: 1},
    new: false,
    update: {$set: {duringInitialSync: true}}
}),
                                      ErrorCodes.IncompleteTransactionHistory);
// Assert that retrying the update fails.
jsTestLog({
    "Secondary": initialSyncedNode,
    "Data": initialSyncedNode.getDB(dbName)['collection'].findOne(),
    "Image": initialSyncedNode.getDB("config")['image_collection'].findOne({"_id.id": lsid}),
    "retried findAndModify against synced node": result
});

result = assert.commandFailedWithCode(initialSyncedNode.getDB(dbName).runCommand({
    findandmodify: primaryColl.getName(),
    lsid: {id: otherLsid},
    txnNumber: NumberLong(3),
    stmtId: NumberInt(1),
    query: {_id: 1},
    new: false,
    remove: true
}),
                                      ErrorCodes.IncompleteTransactionHistory);
// Assert that retrying the delete fails.
jsTestLog({
    "Secondary": initialSyncedNode,
    "Data": initialSyncedNode.getDB(dbName)['collection'].findOne(),
    "Image": initialSyncedNode.getDB("config")['image_collection'].findOne({"_id.id": otherLsid}),
    "retried delete against synced node": result
});

// There should be two sessions/image entries on the initial syncing node, and both should be
// flagged as invalidated.
assert.eq(2, initialSyncedNode.getDB('config')['image_collection'].count({invalidated: true}));
assert.eq(2,
          initialSyncedNode.getDB('config')['image_collection'].count(
              {invalidatedReason: "initial sync"}));

rst.stopSet();
}());
