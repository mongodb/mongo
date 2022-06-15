/**
 * This test serves to ensure that the oplog batcher behavior correctly processes large transactions
 * so that it does not cause any correctness problems.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

// Declare constants.
const DB_NAME = "db_large_txn_correctness";
const COLL_NAME = "db_large_txn_correctness";

// Make a large document of size 'numMB' so that it can easily fill up an oplog entry.
const makeLargeDoc = numMB => new Array(numMB * 1024 * 1024).join('a');

// Spin up a replica set.
const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();

const session = primary.startSession();

// Creating a collection so the first test can just test if regular CRUD operations work.
session.getDatabase(DB_NAME).createCollection(COLL_NAME);

let commitRes;

try {
    // Perform a large transaction (>16MB) with only CRUD operations to ensure that nothing
    // fundamental is broken.
    session.startTransaction();
    session.getDatabase(DB_NAME)[COLL_NAME].insert({doc: makeLargeDoc(10)});
    session.getDatabase(DB_NAME)[COLL_NAME].insert({doc: makeLargeDoc(10)});
    commitRes = session.commitTransaction_forTesting();
    assert.eq(1, commitRes.ok);

    // Ensure that the collection has been dropped so that collection creation can be tested
    // in a txn.
    session.getDatabase(DB_NAME)[COLL_NAME].drop();

    // Create a large transaction (>16MB) with a command and ensure that it works.
    session.startTransaction();
    session.getDatabase(DB_NAME).createCollection(COLL_NAME);
    session.getDatabase(DB_NAME)[COLL_NAME].insert({doc: makeLargeDoc(10)});
    session.getDatabase(DB_NAME)[COLL_NAME].insert({doc: makeLargeDoc(10)});
    commitRes = session.commitTransaction_forTesting();
    assert.eq(1, commitRes.ok);
} finally {
    session.endSession();
    replSet.stopSet();
}
})();
