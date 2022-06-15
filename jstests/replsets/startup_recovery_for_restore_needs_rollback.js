/*
 * Tests that if we recover from a node with a lagged stable timestamp using the special
 * "for restore" mode, and there was a rollback within the recovered oplog, that we crash rather
 * than attempt to use the node.
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

// The restore node is made non-voting so the majority is 2.
// Disable primary catch up since we want to force a rollback.
const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}, {}, {}],
    settings: {catchUpTimeoutMillis: 0, chainingAllowed: false}
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

const nExtraDocs = 50;
jsTestLog("Inserting " + nExtraDocs + " documents with majority point held back.");
bulk = coll.initializeUnorderedBulkOp();
const lastId = nDocs + nExtraDocs;
for (let id = 1; id <= nExtraDocs; id++) {
    bulk.insert({_id: (id + nDocs), paddingStr: paddingStr});
}
bulk.execute();
rst.awaitReplication(undefined, undefined, [restoreNode]);

// Stop some nodes so we can force a rollback
rst.stop(primary);
rst.stop(restoreNode, undefined, undefined, {forRestart: true});

// Restart replication and step up a new primary.
stopReplProducer2.off();
stopReplProducer3.off();

const newPrimary = nodes[2];
// Must send stepUp command without using ReplSetTest helper, as ReplSetTest helper expects all
// nodes to be alive.
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
rst.awaitNodesAgreeOnPrimary(undefined, [nodes[2], nodes[3]]);
assert.soon(() => (rst.getPrimary() == newPrimary));

// Write some stuff to force a rollback
assert.commandWorked(newPrimary.getDB(dbName)[collName].insert({_id: "ForceRollback"}));
rst.awaitReplication(undefined, undefined, [nodes[3]]);

// Bring the new node up in startupRecoveryForRestore mode.  Since it can't see the set, this
// should succeed.

jsTestLog("Restarting restore node with the --startupRecoveryForRestore flag");
clearRawMongoProgramOutput();
restoreNode = rst.start(
    restoreNode,
    {
        noReplSet: true,
        setParameter: Object.merge(startParams, {
            startupRecoveryForRestore: true,
            recoverFromOplogAsStandalone: true,
            takeUnstableCheckpointOnShutdown: true,
            'failpoint.hangAfterCollectionInserts':
                tojson({mode: 'alwaysOn', data: {collectionNS: sentinelColl.getFullName()}}),

        })
    },
    true /* restart */);
// Make sure we can read the last doc after standalone recovery.
assert.docEq({_id: lastId, paddingStr: paddingStr},
             restoreNode.getDB(dbName)[collName].findOne({_id: lastId}));

clearRawMongoProgramOutput();
jsTestLog("Restarting restore node again, in repl set mode");
restoreNode = rst.restart(restoreNode, {noReplSet: false, setParameter: startParams});

// This node should not come back up, because it has no stable timestamp to recover to.
assert.soon(() => (rawMongoProgramOutput().search("UnrecoverableRollbackError") >= 0));
// Hide the exit code from stopSet.
waitMongoProgram(parseInt(restoreNode.port));

// Remove the nodes which are down.
rst.remove(primary);
rst.remove(restoreNode);
// Shut down the set.
rst.stopSet();
})();
