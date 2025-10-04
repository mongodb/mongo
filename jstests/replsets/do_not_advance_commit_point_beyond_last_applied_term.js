/**
 * Tests that secondaries do not advance their commit point to have a greater term than their
 * lastApplied. This prevents incorrectly prefix-committing operations that are not majority
 * committed.
 * @tags: [requires_majority_read_concern]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const dbName = "test";
const collName = "coll";

// Set up a ReplSetTest where nodes only sync one oplog entry at a time.
const rst = new ReplSetTest({nodes: 5, useBridge: true, nodeOptions: {setParameter: "bgSyncOplogFetcherBatchSize=1"}});
rst.startSet();
const config = rst.getReplSetConfig();
// Prevent elections.
config.settings = {
    electionTimeoutMillis: 12 * 60 * 60 * 1000,
};
rst.initiate(config);

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    rst.getPrimary().adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

const nodeA = rst.nodes[0];
const nodeB = rst.nodes[1];
const nodeC = rst.nodes[2];
const nodeD = rst.nodes[3];
const nodeE = rst.nodes[4];

jsTest.log("Node A is primary in term 1. Node E is delayed.");
// A: [1]
// B: [1]
// C: [1]
// D: [1]
// E:
assert.eq(nodeA, rst.getPrimary());
rst.waitForConfigReplication(nodeA);
nodeE.disconnect([nodeA, nodeB, nodeC, nodeD]);
assert.commandWorked(nodeA.getDB(dbName)[collName].insert({term: 1}));
rst.awaitReplication(undefined, undefined, [nodeB, nodeC, nodeD]);

jsTest.log("Node B steps up in term 2 and performs a write, which is not replicated.");
// A: [1]
// B: [1] [2]
// C: [1]
// D: [1]
// E:
stopServerReplication([nodeA, nodeC, nodeD]);
assert.commandWorked(nodeB.adminCommand({replSetStepUp: 1}));
rst.awaitSecondaryNodes(null, [nodeA]);
assert.eq(nodeB, rst.getPrimary());
assert.commandWorked(nodeB.getDB(dbName)[collName].insert({term: 2}));
rst.waitForConfigReplication(nodeB, [nodeA, nodeB, nodeC, nodeD]);

jsTest.log("Node A steps up again in term 3 with votes from A, C, and D and commits a write.");
// A: [1] [3]
// B: [1] [2]
// C: [1] [3]
// D: [1] [3]
// E:
nodeB.disconnect([nodeA, nodeC, nodeD, nodeE]);
assert.commandWorked(nodeA.adminCommand({replSetStepUp: 1}));
restartServerReplication([nodeA, nodeC, nodeD]);
assert.soon(() => {
    // We cannot use getPrimary() here because 2 nodes report they are primary.
    return assert.commandWorked(nodeA.adminCommand({hello: 1})).isWritablePrimary;
});
assert.commandWorked(nodeA.getDB(dbName)[collName].insert({term: 3}, {writeConcern: {w: "majority"}}));
rst.awaitReplication(1000, undefined, [nodeA, nodeC, nodeD], undefined, nodeA);
assert.eq(1, nodeC.getDB(dbName)[collName].find({term: 3}).itcount());
assert.eq(1, nodeD.getDB(dbName)[collName].find({term: 3}).itcount());

jsTest.log("Node E syncs from a majority node and learns the new commit point in term 3.");
// A: [1] [3]
// B: [1] [2]
// C: [1] [3]
// D: [1] [3]
// E: [1]
// The stopReplProducerOnDocument failpoint ensures that Node E stops replicating before
// applying the document {msg: "new primary"}, which is the first document of term 3. This
// depends on the oplog fetcher batch size being 1.
const failPoint = configureFailPoint(nodeE, "stopReplProducerOnDocument", {document: {msg: "new primary"}});
nodeE.reconnect([nodeA, nodeC, nodeD]);
failPoint.wait();

assert.soon(() => {
    return 1 === nodeE.getDB(dbName)[collName].find({term: 1}).itcount();
});
assert.eq(0, nodeE.getDB(dbName)[collName].find({term: 3}).itcount());

jsTest.log("Node E switches its sync source to B and replicates the stale branch of term 2.");
nodeE.disconnect([nodeA, nodeC, nodeD]);
nodeB.reconnect(nodeE);
rst.awaitSyncSource(nodeE, nodeB);
failPoint.off();
assert.soon(() => {
    return 1 === nodeE.getDB(dbName)[collName].find({term: 2}).itcount();
});

jsTest.log("Node E must not return the entry in term 2 as committed.");
assert.eq(0, nodeE.getDB(dbName)[collName].find({term: 2}).readConcern("majority").itcount());

jsTest.log("Reconnect the set. Node E must roll back successfully.");
nodeE.reconnect([nodeA, nodeC, nodeD]);
nodeB.reconnect([nodeA, nodeC, nodeD]);
rst.awaitReplication();
assert.eq(1, nodeE.getDB(dbName)[collName].find({term: 1}).itcount());
assert.eq(0, nodeE.getDB(dbName)[collName].find({term: 2}).itcount());
assert.eq(1, nodeE.getDB(dbName)[collName].find({term: 3}).itcount());

rst.stopSet();
