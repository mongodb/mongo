/**
 * Tests commit point propagation behavior for oplog exhaust cursors. Specifically, we make sure
 * commit point propagation is not affected by lastCommittedOpTime temporarily being null (on either
 * the sync source node or the syncing node).
 *
 * @tags: [
 *  requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const dbName = jsTest.name();
const collName = "foo";

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
let primaryColl = primary.getDB(dbName)[collName];

// Do an initial write and wait for it to be committed on both nodes to advance the commit point and
// the stable timestamp to reflect the user write. This simulates a healthy replica set.
jsTestLog("Do a document write and wait for the commit point to advance on both nodes");
assert.commandWorked(primaryColl.insert({_id: -1}, {"writeConcern": {"w": "majority"}}));
rst.awaitLastOpCommitted();

let startupParams = {};
startupParams["logComponentVerbosity"] = tojson({replication: 2});
startupParams["failpoint.pauseJournalFlusherThread"] = tojson({mode: "alwaysOn"});

// Restart both nodes at the same time and pause the JournalFlusher. The lastCommittedOpTime for
// both nodes should be uninitialized after the restart until the first journal flush.
jsTestLog("Shutting down the replica set for restart");
rst.stopSet(null /* signal */, true /*forRestart */);

jsTestLog("Restarting the replica set with JournalFlusher thread paused");
const nodes = rst.startSet({restart: true, setParameter: startupParams});

// Step up the first node to speed up the test instead of waiting out the election timeout.
rst.stepUp(nodes[0], {awaitReplicationBeforeStepUp: false});

primary = rst.getPrimary();
secondary = rst.getSecondary();
primaryColl = primary.getDB(dbName)[collName];

// Wait for the restarted secondary to establish an exhaust oplog cursor while both nodes'
// lastCommittedOpTime is null.
jsTestLog("Waiting for the restarted secondary to select a sync source and run an oplog getMore");
assert.soon(
    () => assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1})).syncSourceId === 0 &&
        assert.commandWorked(secondary.adminCommand({serverStatus: 1}))
                .metrics.repl.network.getmores.num >= 1,
    "Timed out waiting for restarted secondary to fetch oplog");

jsTestLog("Resume JournalFlusher thread on both nodes");
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseJournalFlusherThread", mode: "off"}));
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "pauseJournalFlusherThread", mode: "off"}));

// Record the numEmptyBatches now before the test.
const numEmptyBatchesBefore =
    secondary.adminCommand({serverStatus: 1}).metrics.repl.network.getmores.numEmptyBatches;
jsTestLog("numEmptyBatches[Before] on secondary is: " + numEmptyBatchesBefore);

// Do some more writes, which will advance the commit point on the primary (sync source) for a
// couple of times.
jsTestLog("Do more writes");
primaryColl = primary.getCollection(primaryColl.getFullName());
for (let i = 0; i < 10; i++) {
    assert.commandWorked(primaryColl.insert({_id: i}, {"writeConcern": {"w": "majority"}}));
    sleep(100);
}

// Test that numEmptyBatches increases after the test as the commit point advances.
const numEmptyBatchesAfter =
    secondary.adminCommand({serverStatus: 1}).metrics.repl.network.getmores.numEmptyBatches;
jsTestLog("numEmptyBatches[After] on secondary now is: " + numEmptyBatchesAfter);

assert(numEmptyBatchesAfter > numEmptyBatchesBefore,
       "Expected empty oplog batches for commit point propagation but got none");

rst.stopSet();
