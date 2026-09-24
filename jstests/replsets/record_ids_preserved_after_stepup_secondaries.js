/*
 * Tests that during secondary oplog application, the secondary keeps track
 * of the highest recordId it has seen, so that on becoming primary, it doesn't
 * reuse recordIds that have already been used.
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = jsTestName();
// The secondary is stepped up below, so it has to be electable. Declare that explicitly
// rather than relying on the default, since some configurations default every node other
// than the first to priority 0.
const replTest = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 1}}]});
replTest.startSet();
replTest.initiate();

const dbName = "test";
const collName = "rrid";

const primary = replTest.getPrimary();
const secondary = replTest.getSecondaries()[0];
const primDB = primary.getDB(dbName);

// Setup: to simulate a situation where the oplog has recordIds that are
// not in an arbitrary order (due to multiple clients inserting in parallel
// on the primary), we're using "applyOps" to artificially create that state.
// Suppose the oplog looks like:
// [
//     {op: i, ts: 1 ..., rid: 3},
//     {op: i, ts: 2 ..., rid: 1},
//     {op: i, ts: 3 ..., rid: 2}
// ]
// For the simulation, we'll have just one oplog entry with recordId 3.
// This is as though before the last two writes got replicated, the
// secondary became the primary.
const ops = [];
ops.push({op: "i", ns: dbName + "." + collName, o: {_id: 1}, o2: {_id: 1}, rid: NumberLong(3)});
assert.commandWorked(primDB.runCommand({create: collName}));
assert.commandWorked(primDB.runCommand({applyOps: ops}));

// Now make the secondary step up. This steps the current primary down rather than stepping the
// secondary up, because a primary that has a node step up underneath it can be forced to step down
// abruptly and leave the set. A commanded step down instead hands the primary role to an electable
// node and leaves the old primary running as a secondary, which this test reads from below.
assert.adminCommandWorkedAllowingNetworkError(primary, {replSetStepDown: 60});
replTest.awaitNodesAgreeOnPrimary(replTest.timeoutMS, replTest.nodes, secondary);
replTest.awaitSecondaryNodes(null, [primary]);
// Agreeing on the primary happens before that node finishes transitioning and starts accepting
// writes, so wait for it to be writable before inserting below.
assert.soon(
    () => assert.commandWorked(secondary.adminCommand({hello: 1})).isWritablePrimary,
    "the new primary never started accepting writes",
);
const newPrimDB = secondary.getDB(dbName);

// Insert documents onto the new primary, and ensure that no original recordIds were reused.
// There should be 3 documents. One from earlier, and then two new ones inserted below.
assert.commandWorked(newPrimDB[collName].insertMany([{_id: 2}, {_id: 3}]));

// Both nodes should have nine documents.
replTest.awaitReplication();
const docsOnNewPrim = newPrimDB[collName].find().toArray();
const docsOnOldPrim = primDB[collName].find().toArray();
assert.eq(3, docsOnNewPrim.length, docsOnNewPrim);
assert.eq(3, docsOnOldPrim.length, docsOnOldPrim);
assert.sameMembers(docsOnNewPrim, docsOnOldPrim);

replTest.stopSet();
