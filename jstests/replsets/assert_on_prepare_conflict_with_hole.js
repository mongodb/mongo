/**
 * Constructs the following cycle that can lead to stalling a sharded cluster:
 * | Preparer                              | Retryable findAndModify   | OplogVisibility Ts |
 * |---------------------------------------+---------------------------+--------------------|
 * | BeginTxn                              |                           |                    |
 * | Write A                               |                           |                    |
 * |                                       | BeginTxn                  |                    |
 * |                                       | Preallocates TS(9),TS(10) |                  8 |
 * | (side txn commits prepare oplog @ 11) |                           |                    |
 * | Prepare 11                            |                           |                    |
 * |                                       | Write A (PrepareConflict) |                    |
 *
 * In this scenario, the prepared transaction blocks waiting for its prepare oplog entry at
 * timestamp 11 to become majority committed. However, the prepare oplog entry cannot replicate to
 * secondaries until the oplog visibility timestamp advances to 11. The oplog visibility timestamp
 * advancing is blocked on the findAndModify that allocated timestamps 9 and 10. The findAndModify
 * cannot make progress because it has hit a prepare conflict. The prepare conflict this test
 * specifically exercises is for duplicate key detection on a non-_id unique index.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

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

// Having changeStreamPreAndPostImages is required to cause the findAndModify to reserve optimes.
assert.commandWorked(
    db.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));

// Insert the document to be updated by findAndModify.
assert.commandWorked(db[collName].insert({a: 3}));

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

// In another thread, perform an retryable find and modify that also attempts to touch the `a: 2`
// document. This findAndModify will block until the above transaction commits or aborts. If the
// above transaction commits, this findAndModify will fail with a duplicate key. If the above
// transaction is aborted, this findAndModify will succeed.
//
// This operation will open up a hole in the oplog preventing writes from becoming majority
// committed. In a properly behaving system, we will notice this resource being held while
// entering a blocking call (prepare conflict resolution) and retry the transaction (which
// releases the resource that prevents writes from becoming majority committed).
const triggerPrepareConflictThread = new Thread(function(host, dbName, collName) {
    const conn = new Mongo(host);
    const session = conn.startSession({retryWrites: true});
    const collection = session.getDatabase(dbName).getCollection(collName);
    jsTestLog("Inserting a conflicting operation while keeping a hole open.");
    assert.throwsWithCode(() => {
        collection.findAndModify(
            {query: {a: 3}, update: {a: 2, fromFindAndModify: true}, upsert: true});
    }, ErrorCodes.DuplicateKey);
}, primary.host, db.getName(), collName);

triggerPrepareConflictThread.start();

// Wait for the findAndModify to be in the system before attempting the majority write. Technically,
// this is insufficient to prove we're properly exercising the code that detects a possible deadlock
// and releases resources. In these cases, the test succeeds because the (yet to happen) majority
// write occurs before the above thread creates a hole.
assert.soon(() => {
    const ops = primary.getDB("admin")
                    .aggregate([
                        {$currentOp: {allUsers: true}},
                        {
                            $match: {
                                type: "op",
                                ns: db[collName].getFullName(),
                                "command.findandmodify": {$exists: true},
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
