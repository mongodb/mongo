/**
 * Tests that the minValid optime being on a divergent branch of history does not impact sync source
 * selection after rollback. See SERVER-59721 for more details.
 *
 * TODO SERVER-49738: remove this test.
 */
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load('jstests/replsets/rslib.js');  // For syncFrom and awaitOpTime.

// Disable primary catchup since this test relies on new primaries not catching up to other nodes.
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: 3, settings: {catchUpTimeoutMillis: 0}, useBridge: true});
const nodes = rst.startSet();
rst.initiateWithHighElectionTimeout();

const collName = jsTestName();
const node0 = rst.getPrimary();
const node1 = rst.getSecondaries()[0];
const node2 = rst.getSecondaries()[1];

const node0DB = node0.getDB("test");
const node0Coll = node0DB.getCollection(collName);

// The default WC is majority and various failpoints used in this test are incompatible with that.
assert.commandWorked(node0.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Make sure node 1 syncs from node 0 so that it will replicate entries that be rolled back.
syncFrom(node1, node0, rst);

jsTestLog("Do write that will become the new majority commit point");
assert.commandWorked(
    node0Coll.insert({_id: "majority committed"}, {writeConcern: {w: "majority"}}));

rst.awaitReplication();

jsTestLog("Disable snapshotting on all nodes");

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach(node => assert.commandWorked(node.adminCommand(
                  {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));

// Stop replication on all nodes. We do this on node 0 and 1 so that they will vote for other nodes
// in future elections. We use a different failpoint for node 1 so that it won't switch sync sources
// when replication is unpaused. We stop replication on node 2 so that it doesn't receive any oplog
// entries from the diverging branch of history.
let node2StopRepl = configureFailPoint(node2, "stopReplProducer");
let node1StopRepl = configureFailPoint(node1, "hangBeforeProcessingSuccessfulBatch");
let node0StopRepl = configureFailPoint(node0, "stopReplProducer");
configureFailPoint(node1, "disableMaxSyncSourceLagSecs");

jsTestLog("Do write that will eventually be rolled back");

assert.commandWorked(node0Coll.insert({_id: "diverging point"}));

node1StopRepl.wait();
node2StopRepl.wait();

assert.commandWorked(node1.adminCommand({clearLog: 'global'}));

jsTestLog("Stepping up node 2");

// Node 2 runs for election. This is needed before node 1 steps up because otherwise it will always
// lose future elections and will not be considered the proper branch of history.
const electionShell = startParallelShell(() => {
    const newPrimary = db.getMongo();
    const rst = new ReplSetTest(newPrimary.host);
    rst.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});
}, node2.port);

jsTestLog("Waiting for node 1 to vote in election");
checkLog.containsJson(node1, 5972100);

jsTestLog("Waiting for node 1 to replicate diverging branch");
node1StopRepl.off();
awaitOpTime(node1, node0);

jsTestLog("Waiting for node 2 to be writable primary");

// Wait for parallelShell to exit. This means that node 2 has successfully transitioned to primary.
electionShell();
assert.eq(rst.getPrimary(), node2);

jsTestLog("Waiting for node 0 to step down");
rst.awaitSecondaryNodes(null, [node0]);

// Node 0 won't replicate node 2's new primary oplog entry, so it can elect node 1 again.
node0StopRepl.wait();

jsTestLog("Stepping node 1 up");

// Step up node 1, which causes an untimestamped write to the minValid collection.
rst.stepUp(node1, {awaitReplicationBeforeStepUp: false});

jsTestLog("Stepping node 2 up");

// Node 0 votes for node 2 in this eleciton. Node 2 is ahead of node 0 because of the previous
// election that it won.
rst.stepUp(node2, {awaitReplicationBeforeStepUp: false});

const node2Coll = node2.getDB("test").getCollection(collName);

node0StopRepl.off();
node2StopRepl.off();

jsTestLog("Doing a write on the proper branch of history");
assert.commandWorked(node2Coll.insert({_id: "proper branch of history"}));

jsTestLog("Waiting for node 1 to complete rollback");
rst.awaitSecondaryNodes();

jsTestLog("Node 1 completed rollback");

// awaitReplication will only succeed if node 1 was able to successfully choose a sync source.
rst.awaitReplication();

assert.eq(node2Coll.find({_id: "proper branch of history"}).itcount(), 1);
assert.eq(node2Coll.find({_id: "diverging point"}).itcount(), 0);

rst.stopSet();
})();