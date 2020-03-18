/* Tests that attempts to create indexes that already exist as of the createIndexes call are
 * permitted inside multi-document transactions. Also test that attempts to create new
 * indexes on existing collections are not permitted inside multi-document transactions.
 *
 * @tags: [uses_transactions,
 *         # Creating indexes inside multi-document transactions is supported only in v4.4 onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

const session = db.getMongo().startSession();
const collName = "create_existing_indexes";

let sessionDB = session.getDatabase("test");
let sessionColl = sessionDB[collName];
sessionColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(sessionDB.createCollection(collName, {writeConcern: {w: "majority"}}));
assert.commandWorked(sessionDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
}));

jsTest.log("Testing createIndexes on an existing index in a transaction");
session.startTransaction({writeConcern: {w: "majority"}});

assert.commandWorked(
    sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));

session.commitTransaction();

jsTest.log("Testing createIndexes on a conflicting index in a transaction (throws error)");
session.startTransaction({writeConcern: {w: "majority"}});

assert.commandFailedWithCode(
    sessionColl.runCommand({createIndexes: collName, indexes: [{key: {a: -1}, name: "a_1"}]}),
    ErrorCodes.IndexKeySpecsConflict);

assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

jsTest.log("Testing createIndexes on a new index in a transaction (throws error)");
session.startTransaction({writeConcern: {w: "majority"}});

assert.commandFailedWithCode(
    sessionColl.runCommand({createIndexes: collName, indexes: [{key: {b: 1}, name: "b_1"}]}),
    ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.endSession();
}());
