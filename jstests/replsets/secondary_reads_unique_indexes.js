/**
 * This test ensures readers on a secondary are unable to read from unique indexes that temporarily
 * violate their uniqueness constraint. When oplog entries are applied in parallel batch writes,
 * operations can be applied out-of-order, and reading at an inconsistent state is possible.
 *
 * For example, take two documents, { _id: 1, x: 1} and  { _id: 2, x: 2} with a unique index on 'x'.
 * On the primary, two updates take place in this order:
 *   { _id: 2, x: 3 }
 *   { _id: 1, x: 2 }
 * There is no uniqueness violation here because x: 3 happens before x: 2. If the updates had
 * occured in the opposite order, a DuplicateKey error would be returned to the user.
 *
 * When these operations are applied on a secondary, they split up across one of 16 writer threads,
 * hashed on the _id of the document. This guarantees that updates to the same document will occur
 * in-order on the same thread.
 *
 * Take two parallel threads applying the same updates as before:
 *      Thread 1            Thread 2
 *   { _id: 1, x: 2 }
 *                       { _id: 2, x: 3 }
 *
 * If a secondary reader were to access the index entry for x=2 after Thread 1 made its update but
 * before Thread 2 made its update, they would find two entries for x=2, which is a violation of the
 * uniqueness constraint. When applying operations in parallel like this, we temporarily ignore
 * uniqueness violations on indexes, and require readers on secondaries to wait for the parallel
 * batch insert to complete, at which point the state of the indexes will be consistent.
 */

(function() {
"use strict";

load("jstests/replsets/libs/secondary_reads_test.js");

const name = "secondaryReadsUniqueIndexes";
const collName = "testColl";
let secondaryReadsTest = new SecondaryReadsTest(name);

let primaryDB = secondaryReadsTest.getPrimaryDB();
let secondaryDB = secondaryReadsTest.getSecondaryDB();

// Setup collection.
primaryDB.runCommand({drop: collName});
assert.commandWorked(primaryDB.runCommand({create: collName}));

// Create a unique index on the collection in the foreground.
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1", unique: true}]}));

let replSet = secondaryReadsTest.getReplset();
replSet.awaitReplication();

// We want to do updates with at least as many different documents as there are parallel batch
// writer threads (16). Each iteration increments and decrements a uniquely indexed value, 'x'.
// The goal is that a reader on a secondary might find a case where the unique index constraint
// is ignored, and an index on x maps to two different records.
const nOps = 16;
const nIterations = 50;
const nReaders = 16;

// Do a bunch of reads using the 'x' index on the secondary.
// No errors should be encountered on the secondary.
let readFn = function() {
    for (let x = 0; x < TestData.nOps; x++) {
        assert.commandWorked(db.runCommand({
            find: TestData.collName,
            filter: {x: x},
            projection: {x: 1},
            readConcern: {level: "local"},
        }));
        // Sleep a bit to make these reader threads less CPU intensive.
        sleep(60);
    }
};
TestData.nOps = nOps;
TestData.collName = collName;
secondaryReadsTest.startSecondaryReaders(nReaders, readFn);

// Write the initial documents. Ensure they have been replicated.
for (let i = 0; i < nOps; i++) {
    assert.commandWorked(
        primaryDB.runCommand({insert: collName, documents: [{_id: i, x: i, iter: 0}]}));
}
replSet.awaitReplication();

// Cycle the value of x in the document {_id: i, x: i} between i and i+1 each iteration.
for (let iteration = 0; iteration < nIterations; iteration++) {
    let updates = [];
    // Reset each document.
    for (let i = 0; i < nOps; i++) {
        updates[i] = {q: {_id: i}, u: {x: i, iter: iteration}};
    }

    assert.commandWorked(primaryDB.runCommand({update: collName, updates: updates}));
    updates = [];

    // Generate updates that increment x on each document backwards by _id to avoid conficts
    // when applied in-order. When these updates get applied to the secondary, they may get
    // applied out of order by different threads and temporarily violate unique index
    // constraints.
    for (let i = 0; i < nOps; i++) {
        // Start at the end and increment x by 1.
        let end = nOps - i - 1;
        let nextX = end + 1;
        updates[i] = {q: {_id: end}, u: {x: nextX, iter: iteration}};
    }
    print("iteration " + iteration);
    assert.commandWorked(primaryDB.runCommand({update: collName, updates: updates}));
}

replSet.awaitReplication();
secondaryReadsTest.stop();
})();
