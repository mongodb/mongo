/*
 * Tests that step up after an initial sync counts recordIds from the right
 * place. In other words, the initial syncing node, if it becomes primary,
 * needs to make sure that it doesn't reuse recordIds that have already been
 * used.
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
import {
    validateShowRecordIdReplicatesAcrossNodes
} from "jstests/libs/replicated_record_ids_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = jsTestName();
const replTest = new ReplSetTest({name: testName, nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 'rrid';

const primary = replTest.getPrimary();
const primDB = primary.getDB(dbName);

// Case 1: Insert documents on the primary where the recordIds start from recordId:2.
// The new node, after initial sync, must also start its recordIds from recordId:2.

// Insert documents where some have a $recordId field within them. The recordId provided
// here is just a field and is separate from the true recordId used when inserting.
assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
assert.commandWorked(primDB[collName].insertMany([
    {_id: 1, a: 1},                 // recordId: 1
    {$recordId: 12, _id: 2, a: 2},  // recordId: 2
    {_id: 3, $recordId: 13, a: 3},  // recordId: 3
    {_id: 4, a: 4}                  // recordId: 4
]));
assert.commandWorked(primDB[collName].remove({_id: 1}));
assert.commandWorked(primDB[collName].insert({_id: 1, a: 1}));
assert.commandWorked(primDB[collName].remove({_id: 1}));
assert.commandWorked(primDB[collName].insert({_id: 1, a: 1}));  // recordId: 6
// At this point, the recordIds of the documents are:
// _id: 1 -> recordId(6)
// _id: 2 -> recordId(2)
// _id: 3 -> recordId(3)
// _id: 4 -> recordId(4)

jsTestLog("Add a new node and wait for it to become secondary.");
const initialSyncNode = replTest.add(
    {setParameter: {logComponentVerbosity: tojsononeline({replication: 5, storage: 5})}});

replTest.reInitiate();
replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
replTest.awaitReplication();

validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);
assert.sameMembers(primary.getDB(dbName).getCollectionInfos(),
                   initialSyncNode.getDB(dbName).getCollectionInfos());

// Now make the initial sync node step up.
replTest.stepUp(initialSyncNode);
const newPrimDB = initialSyncNode.getDB(dbName);

// Insert documents onto the new primary, and ensure that no original recordIds were reused.
// There should be 9 documents - 4 from earlier and another 5 now.
assert.commandWorked(newPrimDB[collName].insertMany([
    {_id: 7},   // recordId: 7
    {_id: 8},   // recordId: 8
    {_id: 9},   // recordId: 9
    {_id: 10},  // recordId: 10
    {_id: 11}   // recordId: 11
]));

// Both nodes should have nine documents.
replTest.awaitReplication();
const docsOnNewPrim = newPrimDB[collName].find().toArray();
const docsOnOldPrim = primDB[collName].find().toArray();
assert.eq(9, docsOnNewPrim.length, docsOnNewPrim);
assert.eq(9, docsOnOldPrim.length, docsOnOldPrim);
assert.sameMembers(docsOnNewPrim, docsOnOldPrim);
validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);

replTest.stopSet();
