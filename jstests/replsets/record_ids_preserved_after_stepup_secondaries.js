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
const replTest = new ReplSetTest({name: testName, nodes: 2});
replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 'rrid';

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
ops.push({op: "i", ns: dbName + '.' + collName, o: {_id: 1}, o2: {_id: 1}, rid: NumberLong(3)});
assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
assert.commandWorked(primDB.runCommand({applyOps: ops}));

// Now make the secondary step up.
replTest.stepUp(secondary);
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
