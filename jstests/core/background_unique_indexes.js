/**
 * This test ensures background index builds are unable to see documents that temporarily violate
 * uniqueness constraints, when potentially applied on a secondary during replication. When oplog
 * entries are applied in parallel batch writes, operations can be applied out-of-order, and reading
 * at an inconsistent state is possible.
 *
 * See SERVER-38675.
 *
 * @tags: [
 *   # Sharding suites that use a hashed shard key on _id cannot create a unique index on {x: 1}.
 *   cannot_create_unique_index_when_using_hashed_shard_key,
 *   requires_background_index
 *   ]
 */

(function() {
"use strict";

const dbName = "background_unique_indexes";
const collName = "test";

let testDB = db.getSiblingDB(dbName);

// Setup collection.
testDB[collName].drop();
assert.commandWorked(testDB.runCommand({create: collName}));

// Each iteration increments and decrements a uniquely indexed value, 'x' while creating and
// dropping an index. The goal is that an index build on a secondary might find a case where the
// unique index constraint is temporarily violated, and an index on x maps to two different
// records.
const nOps = 1000;
const nIterations = 15;

// Write the initial documents.
let bulk = testDB[collName].initializeUnorderedBulkOp();
for (let i = 0; i < nOps; i++) {
    bulk.insert({_id: i, x: i, iter: 0});
}
assert.commandWorked(bulk.execute());

// Cycle the value of x in the document {_id: i, x: i} between i and i+1 each iteration.
for (let iteration = 0; iteration < nIterations; iteration++) {
    // Reset each document.
    let updates = [];
    for (let i = 0; i < nOps; i++) {
        updates[i] = {q: {_id: i}, u: {x: i, iter: iteration}};
    }

    assert.commandWorked(testDB.runCommand({update: collName, updates: updates}));

    // Create a background unique index on the collection.
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {x: 1}, name: "x_1", background: true, unique: true}]
    }));

    // Generate updates that increment x on each document backwards by _id to avoid conficts
    // when applied in-order.
    updates = [];
    for (let i = 0; i < nOps; i++) {
        // Apply each operation in reverse order.
        updates[nOps - i - 1] = {q: {_id: i}, u: {$inc: {x: 1}}};
    }
    assert.commandWorked(testDB.runCommand({update: collName, updates: updates}));

    assert.commandWorked(testDB.runCommand({dropIndexes: collName, index: "x_1"}));
    print("iteration " + iteration);
}
})();
