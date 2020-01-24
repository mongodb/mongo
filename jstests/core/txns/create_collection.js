/* Tests simple cases of creating a collection inside a multi-document transaction, both
 * committing and aborting.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/create_collection_txn_helpers.js");

const session = db.getMongo().startSession({causalConsistency: false});
const collName = "create_new_collection";
const secondCollName = collName + "_second";

let sessionDB = session.getDatabase("test");
let sessionColl = sessionDB[collName];
let secondSessionColl = sessionDB[secondCollName];
sessionColl.drop({writeConcern: {w: "majority"}});
secondSessionColl.drop({writeConcern: {w: "majority"}});

jsTest.log("Testing createCollection in a transaction");
session.startTransaction({writeConcern: {w: "majority"}});
createCollAndCRUDInTxn(sessionDB, collName);
session.commitTransaction();
assert.eq(sessionColl.find({}).itcount(), 1);

sessionColl.drop({writeConcern: {w: "majority"}});
jsTest.log("Testing multiple createCollections in a transaction");
session.startTransaction({writeConcern: {w: "majority"}});
createCollAndCRUDInTxn(sessionDB, collName);
createCollAndCRUDInTxn(sessionDB, secondCollName);
session.commitTransaction();
assert.eq(sessionColl.find({}).itcount(), 1);
assert.eq(secondSessionColl.find({}).itcount(), 1);

sessionColl.drop({writeConcern: {w: "majority"}});
secondSessionColl.drop({writeConcern: {w: "majority"}});

jsTest.log("Testing createCollection in a transaction that aborts");
session.startTransaction({writeConcern: {w: "majority"}});
createCollAndCRUDInTxn(sessionDB, collName);
assert.commandWorked(session.abortTransaction_forTesting());

assert.eq(sessionColl.find({}).itcount(), 0);

assert.commandWorked(sessionDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
jsTest.log(
    "Testing createCollection on an existing collection in a transaction that aborts (SHOULD FAIL)");
session.startTransaction({writeConcern: {w: "majority"}});
createCollAndCRUDInTxn(sessionDB, secondCollName);
assert.commandFailedWithCode(sessionDB.runCommand({create: collName}), ErrorCodes.NamespaceExists);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
assert.eq(sessionColl.find({}).itcount(), 0);
assert.eq(secondSessionColl.find({}).itcount(), 0);

session.endSession();
}());
