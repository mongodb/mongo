/*
 * Tests that we can recover from a node with a lagged stable timestamp using the special
 * "for restore" mode, but not read from older points-in-time on the recovered node, and that
 * we can do so even after we crash in the middle of an attempt to restore.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [requires_wiredtiger, requires_persistence, requires_replication,
 * requires_majority_read_concern, uses_transactions, uses_prepare_transaction,
 * # We don't expect to do this while upgrading.
 * multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
const SIGKILL = 9;

const dbName = TestData.testName;
const logLevel = tojson({storage: {recovery: 2, wt: {wtCheckpoint: 1}}});

const rst = new ReplSetTest({
    nodes: [{}, {}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false}
});

const startParams = {
    logComponentVerbosity: logLevel,
    replBatchLimitOperations: 100
};
const nodes = rst.startSet({setParameter: startParams});
let restoreNode = nodes[1];
rst.initiateWithHighElectionTimeout();
const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const collName = "testcoll";
const sentinelCollName = "sentinelcoll";
const coll = db[collName];
const sentinelColl = db[sentinelCollName];
const paddingStr = "XXXXXXXXX";

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Pre-load some documents.

const nDocs = 100;
let bulk = coll.initializeUnorderedBulkOp();
for (let id = 1; id <= nDocs; id++) {
    bulk.insert({_id: id, paddingStr: paddingStr});
}
bulk.execute();
rst.awaitReplication();

const holdOpTime = assert.commandWorked(db.runCommand({find: collName, limit: 1})).operationTime;

// Keep the stable timestamp from moving on the node we're going to restart in restore mode.
assert.commandWorked(restoreNode.adminCommand({
    configureFailPoint: 'holdStableTimestampAtSpecificTimestamp',
    mode: 'alwaysOn',
    data: {"timestamp": holdOpTime}
}));

// Do a bunch of updates, which are chosen so if one is missed, we'll know.
bulk = coll.initializeUnorderedBulkOp();
const nUpdates = 1000;
const writeSentinelsAfter = 650;  // This should be set to get us to the middle of a batch.
jsTestLog("Making " + nUpdates + " updates with snapshotting disabled on one node.");
for (let updateNo = 1; updateNo <= nUpdates; updateNo++) {
    let id = (updateNo - 1) % nDocs + 1;
    let updateField = "u" + updateNo;
    let setDoc = {};
    setDoc[updateField] = updateNo;
    bulk.find({_id: id}).updateOne({"$set": setDoc});
    if (updateNo == writeSentinelsAfter) {
        //  Write a bunch of inserts to the sentinel collection, which will be used to hang
        // oplog application mid-batch.
        bulk.execute();
        bulk = sentinelColl.initializeUnorderedBulkOp();
        for (let j = 1; j <= 100; j++) {
            bulk.insert({_id: j});
        }
        bulk.execute();
        bulk = coll.initializeUnorderedBulkOp();
    }
}
bulk.execute();
rst.awaitReplication();

jsTestLog("Stopping replication on secondaries to hold back majority commit point.");
let stopReplProducer2 = configureFailPoint(nodes[2], 'stopReplProducer');
let stopReplProducer3 = configureFailPoint(nodes[3], 'stopReplProducer');

const nExtraDocs = 50;
jsTestLog("Inserting " + nExtraDocs + " documents with majority point held back.");
bulk = coll.initializeUnorderedBulkOp();
const lastId = nDocs + nExtraDocs;
for (let id = 1; id <= nExtraDocs; id++) {
    bulk.insert({_id: (id + nDocs), paddingStr: paddingStr});
}
bulk.execute();

const penultimateOpTime =
    assert.commandWorked(db.runCommand({find: collName, limit: 1})).operationTime;

const sentinel2Timestamp =
    assert.commandWorked(db.runCommand({insert: sentinelCollName, documents: [{_id: "s2"}]}))
        .operationTime;

rst.awaitReplication(undefined, undefined, [restoreNode]);

jsTestLog("Restarting restore node with the --startupRecoveryForRestore flag");
// Must use stop/start with waitForConnect: false.  See SERVER-56446
rst.stop(restoreNode, undefined, undefined, {forRestart: true});
clearRawMongoProgramOutput();
rst.start(restoreNode,
          {
              noReplSet: true,
              waitForConnect: false,
              syncdelay: 1,  // Take a lot of unstable checkpoints.
              setParameter: Object.merge(startParams, {
                  startupRecoveryForRestore: true,
                  recoverFromOplogAsStandalone: true,
                  takeUnstableCheckpointOnShutdown: true,
                  'failpoint.hangAfterCollectionInserts':
                      tojson({mode: 'alwaysOn', data: {collectionNS: sentinelColl.getFullName()}}),

              })
          },
          true /* restart */);
assert.soon(() => {  // Can't use checklog because we can't connect to the mongo in startup mode.
    return rawMongoProgramOutput().search("hangAfterCollectionInserts fail point enabled") !== -1;
});
// We need to make sure we get a checkpoint after the failpoint is hit, so we clear the output after
// hitting it.  Occasionally we'll miss a checkpoint as a result of clearing the output, but we'll
// get another one a second later.
clearRawMongoProgramOutput();
// Ensure the checkpoint starts after the insert.
assert.soon(() => {
    return rawMongoProgramOutput().search("WT_VERB_CHECKPOINT.*saving checkpoint snapshot min") !==
        -1;
});
// Ensure that we wait for a checkpoint completed log message that comes strictly after the above
// checkpoint started message.
clearRawMongoProgramOutput();
assert.soon(() => {
    return rawMongoProgramOutput().search("Completed unstable checkpoint.") !== -1;
});

jsTestLog("Restarting restore node uncleanly");
rst.stop(restoreNode, SIGKILL, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
restoreNode = rst.start(restoreNode,
                        {
                            noReplSet: true,
                            setParameter: Object.merge(startParams, {
                                startupRecoveryForRestore: true,
                                recoverFromOplogAsStandalone: true,
                                takeUnstableCheckpointOnShutdown: true
                            })
                        },
                        true /* restart */);
// Make sure we can read something after standalone recovery.
assert.eq(1, restoreNode.getDB(dbName)[sentinelCollName].find({}).limit(1).itcount());

jsTestLog("Restarting restore node again, in repl set mode");
restoreNode = rst.restart(restoreNode, {noReplSet: false, setParameter: startParams});

rst.awaitSecondaryNodes(undefined, [restoreNode]);
jsTestLog("Finished restarting restore node");

// For the timestamp check we step up the restored node so we can do atClusterTime reads on it.
// They are necessarily speculative because we are preventing majority optimes from advancing.

jsTestLog("Stepping up restore node");
rst.stepUp(restoreNode, {awaitReplicationBeforeStepUp: false});

// Should NOT able to read at the penultimate optime on the restore node.
const restoreNodeSession = restoreNode.startSession({causalConsistency: false});
const restoreNodeSessionDb = restoreNodeSession.getDatabase(dbName);
jsTestLog(
    "Checking restore node top-of-oplog minus 1 read, which should fail, because the restore node does not have that history.");
restoreNodeSession.startTransaction(
    {readConcern: {level: "snapshot", atClusterTime: penultimateOpTime}});
assert.commandFailedWithCode(
    restoreNodeSessionDb.runCommand({find: collName, filter: {"_id": lastId}}),
    ErrorCodes.SnapshotTooOld);
restoreNodeSession.abortTransaction();

// Should see that very last document.
assert.docEq({_id: "s2"}, restoreNode.getDB(dbName)[sentinelCollName].findOne({_id: "s2"}));

// Allow set to become current and shut down with ordinary dbHash verification.
stopReplProducer2.off();
stopReplProducer3.off();
rst.stopSet();
})();
