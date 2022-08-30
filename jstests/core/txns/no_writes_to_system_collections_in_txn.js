// Tests that it is illegal to write to system collections within a transaction.
// @tags: [uses_transactions, uses_snapshot_read_concern]
(function() {
"use strict";

const session = db.getMongo().startSession();

// Use a custom database, to avoid conflict with other tests that use the system.js collection.
const testDB = session.getDatabase("no_writes_system_collections_in_txn");
assert.commandWorked(testDB.dropDatabase());
const systemCollName = "system.js";
const systemColl = testDB.getCollection(systemCollName);
const systemDotViews = testDB.getCollection("system.views");

// createCollection is not presently allowed to run in transactions that have a non-local
// readConcern.
// TODO(SERVER-46971) Replace "local" with "snapshot".
session.startTransaction({readConcern: {level: "local"}});
assert.commandFailedWithCode(testDB.createCollection(systemCollName),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// createIndexes is not presently allowed to run in transactions that have a non-local
// readConcern.
// TODO(SERVER-46971) Replace "local" with "snapshot".
session.startTransaction({readConcern: {level: "local"}});
assert.commandFailedWithCode(systemColl.createIndex({name: 1}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// Ensure that a collection exists with at least one document.
assert.commandWorked(systemColl.insert({name: 0}, {writeConcern: {w: "majority"}}));

session.startTransaction({readConcern: {level: "snapshot"}});
let error = assert.throws(() => systemColl.findAndModify({query: {}, update: {}}));
assert.commandFailedWithCode(error, 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
error = assert.throws(() => systemColl.findAndModify({query: {}, remove: true}));
assert.commandFailedWithCode(error, 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(systemColl.insert({name: "new"}), 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(testDB.getCollection("system.profile").insert({name: "new"}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(systemDotViews.insert({_id: "new.view", viewOn: "bar", pipeline: []}),
                             50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(systemColl.update({name: 0}, {$set: {name: "jungsoo"}}), 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(
    systemColl.update({name: "nonexistent"}, {$set: {name: "jungsoo"}}, {upsert: true}), 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(systemColl.remove({name: 0}), 50791);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

assert.commandWorked(systemColl.remove({_id: {$exists: true}}));
assert.eq(systemColl.find().itcount(), 0);
}());
