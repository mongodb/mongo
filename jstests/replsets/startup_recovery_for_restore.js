/*
 * Tests that we can recover from a node with a lagged stable timestamp using the special
 * "for restore" mode, but not read from older points-in-time on the recovered node.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [requires_persistence, requires_replication,
 * requires_majority_read_concern, uses_transactions, uses_prepare_transaction,
 * # We don't expect to do this while upgrading.
 * multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

const dbName = TestData.testName;

const logLevel = tojson({storage: {recovery: 2}});

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
const paddingStr = "XXXXXXXXX";

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Pre-load some documents.
const nPreDocs = 2;
coll.insert([{_id: "pre1"}, {_id: "pre2"}]);
rst.awaitReplication();

const holdOpTime = assert.commandWorked(db.runCommand({find: collName, limit: 1})).operationTime;

// Keep the stable timestamp from moving on the node we're going to restart in restore mode.
assert.commandWorked(restoreNode.adminCommand({
    configureFailPoint: 'holdStableTimestampAtSpecificTimestamp',
    mode: 'alwaysOn',
    data: {"timestamp": holdOpTime}
}));

// Insert a bunch of documents.
let bulk = coll.initializeUnorderedBulkOp();
const nDocs = 1000;
jsTestLog("Inserting " + nDocs + " documents with snapshotting disabled on one node.");
for (let id = 1; id <= nDocs; id++) {
    bulk.insert({_id: id, paddingStr: paddingStr});
}
bulk.execute();
rst.awaitReplication();

jsTestLog("Stopping replication on secondaries to hold back majority commit point.");
let stopReplProducer2 = configureFailPoint(nodes[2], 'stopReplProducer');
let stopReplProducer3 = configureFailPoint(nodes[3], 'stopReplProducer');

jsTestLog("Writing first sentinel document.");
const sentinel1Timestamp =
    assert.commandWorked(db.runCommand({insert: sentinelCollName, documents: [{_id: "s1"}]}))
        .operationTime;

const nExtraDocs = 50;
jsTestLog("Inserting " + nExtraDocs + " documents with majority point held back.");
bulk = coll.initializeUnorderedBulkOp();
for (let id = 1; id <= nExtraDocs; id++) {
    bulk.insert({_id: (id + nDocs), paddingStr: paddingStr});
}
bulk.execute();
const lastId = nDocs + nExtraDocs;

const penultimateOpTime =
    assert.commandWorked(db.runCommand({find: collName, limit: 1})).operationTime;

const sentinel2Timestamp =
    assert.commandWorked(db.runCommand({insert: sentinelCollName, documents: [{_id: "s2"}]}))
        .operationTime;

rst.awaitReplication(undefined, undefined, [restoreNode]);

jsTestLog("Restarting restore node with the --startupRecoveryForRestore flag");
restoreNode = rst.restart(restoreNode, {
    noReplSet: true,
    setParameter: Object.merge(startParams, {
        startupRecoveryForRestore: true,
        recoverFromOplogAsStandalone: true,
        takeUnstableCheckpointOnShutdown: true
    })
});
// Make sure we can read something after standalone recovery.
assert.eq(2, restoreNode.getDB(dbName)[sentinelCollName].find({}).itcount());

// Test that appliedThrough still exists in the minValid document after standalone recovery with
// takeUnstableCheckpointOnShutdown.
let minValid = restoreNode.getCollection('local.replset.minvalid').findOne();
assert(minValid.hasOwnProperty('begin'), tojson(minValid));

jsTestLog("Restarting restore node again, in repl set mode with stable checkpointing disabled");
restoreNode = rst.restart(restoreNode, {
    noReplSet: false,
    setParameter:
        Object.merge(startParams, {"failpoint.disableSnapshotting": "{'mode':'alwaysOn'}"})
});

rst.awaitSecondaryNodes(undefined, [restoreNode]);
jsTestLog("Finished restarting restore node with stable checkpointing disabled");

// Test that no appliedThrough in the minValid document after replication recovery.
minValid = restoreNode.getCollection('local.replset.minvalid').findOne();
assert(!minValid.hasOwnProperty('begin'), tojson(minValid));

// Test that we cannot take another checkpoint via fsync before we have an up-to-date stable
// timestamp because otherwise we would end up mistakenly taking an unstable timestamp with a null
// appliedThrough.
jsTestLog("Running fsync on restore node before it takes the first stable checkpoint should fail");
assert.commandFailed(restoreNode.adminCommand({fsync: 1}));

jsTestLog("Doing more write on the primary");
assert.commandWorked(db.runCommand({insert: sentinelCollName, documents: [{_id: "s3"}]}));

// Make sure we can read the new write on the restore node.  Must be durable because we're about
// to crash this node with no checkpoints.
rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE, [restoreNode]);
assert.eq(3, restoreNode.getDB(dbName)[sentinelCollName].find({}).itcount());

jsTestLog("Crashing restore node before it takes the first stable checkpoint");
rst.stop(restoreNode, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

jsTestLog("Restarting restore node again, in repl set mode");
restoreNode =
    rst.start(restoreNode, {noReplSet: false, setParameter: startParams}, /* restart */ true);

rst.awaitSecondaryNodes(undefined, [restoreNode]);
jsTestLog("Finished restarting restore node");

// Make sure we can still read the latest write on the restore node.
assert.eq(3, restoreNode.getDB(dbName)[sentinelCollName].find({}).itcount());

const restoreDb = restoreNode.getDB(dbName);

jsTestLog("Checking restore node untimestamped read.");
// Basic test: should see all docs with untimestamped read.
assert.eq(nPreDocs + nDocs + nExtraDocs, coll.find().itcount());
assert.eq(nPreDocs + nDocs + nExtraDocs, restoreDb[collName].find().itcount());

// For the remaining checks we step up the restored node so we can do atClusterTime reads on it.
// They are necessarily speculative because we are preventing majority optimes from advancing.

jsTestLog("Stepping up restore node");
rst.stepUp(restoreNode, {awaitReplicationBeforeStepUp: false});

// Should also be able to read at the final sentinel optime on restore node.
const restoreNodeSession = restoreNode.startSession({causalConsistency: false});
restoreNodeSession.startTransaction(
    {readConcern: {level: "snapshot", atClusterTime: sentinel2Timestamp}});
const restoreNodeSessionDb = restoreNodeSession.getDatabase(dbName);
jsTestLog("Checking top-of-oplog read works on restored node.");

let res = assert.commandWorked(
    restoreNodeSessionDb.runCommand({find: collName, filter: {"_id": lastId}}));
assert.eq(1, res.cursor.firstBatch.length);
assert.docEq({_id: lastId, paddingStr: paddingStr}, res.cursor.firstBatch[0]);

// Must abort because majority is not advancing.
restoreNodeSession.abortTransaction();

// Should NOT able to read at the first sentinel optime on the restore node.
restoreNodeSession.startTransaction(
    {readConcern: {level: "snapshot", atClusterTime: sentinel1Timestamp}});
jsTestLog(
    "Checking restore node majority optime read, which should fail, because the restore node does not have that history.");
res = assert.commandFailedWithCode(
    restoreNodeSessionDb.runCommand({find: collName, filter: {"_id": {"$gte": nDocs}}}),
    ErrorCodes.SnapshotTooOld);
restoreNodeSession.abortTransaction();

// Should NOT able to read at the penultimate optime on the restore node either.
jsTestLog(
    "Checking restore node top-of-oplog minus 1 read, which should fail, because the restore node does not have that history.");
restoreNodeSession.startTransaction(
    {readConcern: {level: "snapshot", atClusterTime: penultimateOpTime}});
res = assert.commandFailedWithCode(
    restoreNodeSessionDb.runCommand({find: collName, filter: {"_id": lastId}}),
    ErrorCodes.SnapshotTooOld);
restoreNodeSession.abortTransaction();

// Allow set to become current and shut down with ordinary dbHash verification.
stopReplProducer2.off();
stopReplProducer3.off();
rst.stopSet();
})();
