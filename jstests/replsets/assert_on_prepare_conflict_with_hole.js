/**
 * Constructs the following cycle that can lead to stalling a sharded cluster:
 * | Preparer                              | Insert                    | OplogVisibility Ts |
 * |---------------------------------------+---------------------------+--------------------|
 * | BeginTxn                              |                           |                    |
 * | Write A                               |                           |                    |
 * |                                       | BeginTxn                  |                    |
 * |                                       | Preallocates TS(10)       |                  9 |
 * | (side txn commits prepare oplog @ 11) |                           |                    |
 * | Prepare 11                            |                           |                    |
 * |                                       | Write A (PrepareConflict) |                    |
 *
 * In this scenario, the prepared transaction blocks waiting for its prepare oplog entry at
 * timestamp 11 to become majority committed. However, the prepare oplog entry cannot replicate to
 * secondaries until the oplog visibility timestamp advances to 11. The oplog visibility timestamp
 * advancing is blocked on the insert that allocated timestamps 10. The insert cannot make progress
 * because it has hit a prepare conflict. The prepare conflict this test specifically exercises is
 * for duplicate key detection on a non-_id unique index.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");

// Use a single node replica set for simplicity. Note that an oplog hole on a single node replica
// will block new writes from becoming majority committed.
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {logComponentVerbosity: tojson({storage: 1})},
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
const db = primary.getDB("test");

const collName = "mycoll";
assert.commandWorked(db.runCommand({create: collName, writeConcern: {w: "majority"}}));
// A secondary unique index requires cursor positioning in WT which can result in hitting a prepare
// conflict.
assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));

// Start a multi-document transaction that inserts an `a: 2` update.
const lsid = ({id: UUID()});
assert.commandWorked(db.runCommand({
    insert: collName,
    documents: [{a: 2}],
    lsid,
    txnNumber: NumberLong(1),
    autocommit: false,
    startTransaction: true,
}));

// Prepare the `a: 2` update.
let prepTs = assert.commandWorked(db.adminCommand({
    prepareTransaction: 1,
    lsid,
    txnNumber: NumberLong(1),
    autocommit: false
}))["prepareTimestamp"];

// In another thread, perform an insert that also attempts to touch the `a: 2` update. This insert
// will block until the above transaction commits or aborts. If the above transaction commits, this
// insert will fail with a duplicate key. If the above transaction is aborted, this insert will
// succeed.
//
// This insert will open up a hole in the oplog preventing writes from becoming majority
// committed. In a properly behaving system, we will notice this resource being held while
// entering a blocking call (prepare conflict resolution) and retry the transaction (which
// releases the resource that prevents writes from becoming majority committed).
const triggerPrepareConflictThread = new Thread(function(host, ns) {
    const conn = new Mongo(host);
    const collection = conn.getCollection(ns);
    jsTestLog("Inserting a conflicting operation while keeping a hole open.");
    assert.commandFailedWithCode(collection.insert([{a: 1}, {a: 2}, {a: 3}]),
                                 ErrorCodes.DuplicateKey);
}, primary.host, db[collName].getFullName());

triggerPrepareConflictThread.start();

// Wait for the insert to be in the system before attempting the majority write. Technically, this
// is insufficient to prove we're properly exercising the code that detects a possible deadlock and
// releases resources. In these cases, the test succeeds because the (yet to happen) majority write
// occurs before the above thread creates a hole.
assert.soon(() => {
    const ops = primary.getDB("admin")
                    .aggregate([
                        {$currentOp: {allUsers: true}},
                        {
                            $match: {
                                type: "op",
                                ns: db[collName].getFullName(),
                                "command.insert": {$exists: true},
                            }
                        }
                    ])
                    .toArray();

    if (ops.length === 0) {
        return false;
    }

    assert.eq(ops.length, 1, ops);
    return true;
});

// If the system is misbehaving, this write will fail to "majority replicate". As noted above, in a
// single node replica set, an operation must be visible in the oplog before it can be considered
// majority replicated.
jsTestLog("Doing the majority write.");
assert.soon(() => {
    assert.commandWorked(db.bla.insert({}, {writeConcern: {w: "majority"}}));
    return true;
});

// We could stop the test here, but by committing the transaction we can also assert that the
// `triggerPrepareConflictThread` sees a `DuplicateKey` error.
jsTestLog({"Committing. CommitTs": prepTs});
assert.commandWorked(db.adminCommand({
    commitTransaction: 1,
    lsid,
    txnNumber: NumberLong(1),
    autocommit: false,
    commitTimestamp: prepTs
}));

triggerPrepareConflictThread.join();

rst.stopSet();
})();
