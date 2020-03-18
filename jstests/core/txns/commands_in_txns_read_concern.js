/* Ensures createCollection and createIndexes are not permitted to run with a readConcern other than
 * `local` inside transactions.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44,
 *         uses_snapshot_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/create_collection_txn_helpers.js");
load("jstests/libs/create_index_txn_helpers.js");

const session = db.getMongo().startSession();
const collName = jsTestName();

let sessionDB = session.getDatabase("test");
let sessionColl = sessionDB[collName];
let otherCollName = jsTestName() + "_other";
let otherColl = sessionDB[otherCollName];
sessionColl.drop({writeConcern: {w: "majority"}});
otherColl.drop({writeConcern: {w: "majority"}});

jsTest.log("Testing createCollection in a transaction with local readConcern");
session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
createCollAndCRUDInTxn(sessionDB, collName, true /*explicitCreate*/, false /*upsert*/);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(sessionColl.find({}).itcount(), 1);

sessionColl.drop({writeConcern: {w: "majority"}});

jsTest.log("Testing createIndexes in a transaction with local readConcern");
session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
createIndexAndCRUDInTxn(sessionDB, collName, false /*explicitCollCreate*/, false /*multikeyIndex*/);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(sessionColl.find({}).itcount(), 1);
assert.eq(sessionColl.getIndexes().length, 2);

sessionColl.drop({writeConcern: {w: "majority"}});
otherColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(otherColl.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.eq(otherColl.find({}).itcount(), 1);

jsTest.log("Testing createCollection in a transaction with local readConcern, with other " +
           "operations preceeding it");
session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
assert.eq(otherColl.find({a: 1}).itcount(), 1);
createCollAndCRUDInTxn(sessionDB, collName, true /*explicitCreate*/, false /*upsert*/);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(sessionColl.find({}).itcount(), 1);

sessionColl.drop({writeConcern: {w: "majority"}});
otherColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(otherColl.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.eq(otherColl.find({}).itcount(), 1);

jsTest.log("Testing createIndexes in a transaction with local readConcern, with other " +
           "operations preceeding it");
session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
assert.eq(otherColl.find({a: 1}).itcount(), 1);
createIndexAndCRUDInTxn(sessionDB, collName, false /*explicitCollCreate*/, false /*multikeyIndex*/);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(sessionColl.find({}).itcount(), 1);
assert.eq(sessionColl.getIndexes().length, 2);

sessionColl.drop({writeConcern: {w: "majority"}});
otherColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(otherColl.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.eq(otherColl.find({}).itcount(), 1);

jsTest.log("Testing createCollection in a transaction with non-local readConcern (SHOULD FAIL)");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
let res = sessionDB.createCollection(collName);
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

jsTest.log("Testing createIndexes in a transaction with non-local readConcern (SHOULD FAIL)");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
res = sessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

otherColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(otherColl.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.eq(otherColl.find({}).itcount(), 1);

jsTest.log("Testing createCollection in a transaction with non-local readConcern, with other " +
           "operations preceeding it (SHOULD FAIL)");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
assert.eq(otherColl.find({a: 1}).itcount(), 1);
res = sessionDB.createCollection(collName);
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

otherColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(otherColl.insert({a: 1}, {writeConcern: {w: "majority"}}));
assert.eq(otherColl.find({}).itcount(), 1);

jsTest.log("Testing createIndexes in a transaction with non-local readConcern, with other " +
           "operations preceeding it (SHOULD FAIL)");
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
assert.eq(otherColl.find({a: 1}).itcount(), 1);
res = sessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.endSession();
}());
