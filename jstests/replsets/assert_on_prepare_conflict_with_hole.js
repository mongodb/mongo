/**
 * Constructs the following cycle that can lead to stalling a sharded cluster:
 * | Preparer                              | VectoredInsert            | OplogVisibility Ts |
 * |---------------------------------------+---------------------------+--------------------|
 * | BeginTxn                              |                           |                    |
 * | Write A                               |                           |                    |
 * |                                       | BeginTxn                  |                    |
 * |                                       | Preallocates TS(10, 11)   |                  9 |
 * | (side txn commits prepare oplog @ 12) |                           |                    |
 * | Prepare 12                            |                           |                    |
 * |                                       | Write A (PrepareConflict) |                    |
 *
 * In this scenario, the prepared transaction blocks waiting for its prepare oplog entry at
 * timestamp 12 to become majority committed. However, the prepare oplog entry cannot replicate to
 * secondaries until the oplog visibility timestamp advances to 12. The oplog visibility timestamp
 * advancing is blocked on the VectoredInsert that allocated timestamps 10 and 11. The
 * VectoredInsert cannot make progress because it has hit a prepare conflict.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

const collName = "mycoll";
assert.commandWorked(db.runCommand({create: collName, writeConcern: {w: "majority"}}));
assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));

const lsid = ({id: UUID()});
assert.commandWorked(db.runCommand({
    insert: collName,
    documents: [{a: 2}],
    lsid,
    txnNumber: NumberLong(1),
    autocommit: false,
    startTransaction: true,
}));

assert.commandWorked(
    db.adminCommand({prepareTransaction: 1, lsid, txnNumber: NumberLong(1), autocommit: false}));

assert.commandFailedWithCode(db[collName].insert([{a: 1}, {a: 2}, {a: 3}]),
                             ErrorCodes.WriteConflict);

rst.stop(primary, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
rst.stopSet();
})();
