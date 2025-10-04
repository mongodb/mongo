/**
 * Tests transactions that are prepared after no writes.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: prepareTransaction.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 *   uses_prepare_transaction
 * ]
 */
const dbName = "test";
const collName = "empty_prepare";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const doc = {
    _id: 1,
    a: 1,
    b: 1,
};
assert.commandWorked(testColl.insert(doc));

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

// ---- Test 1. No operations before prepare ----

session.startTransaction();
assert.commandFailedWithCode(
    sessionDB.adminCommand({prepareTransaction: 1}),
    ErrorCodes.OperationNotSupportedInTransaction,
);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// ---- Test 2. Only reads before prepare ----

session.startTransaction();
assert.eq(doc, sessionColl.findOne({a: 1}));
let res = assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
// Makes sure prepareTransaction returns prepareTimestamp in its response.
assert(res.hasOwnProperty("prepareTimestamp"), tojson(res));
assert.commandWorked(session.abortTransaction_forTesting());

// ---- Test 3. Noop writes before prepare ----

session.startTransaction();
res = assert.commandWorked(sessionColl.update({a: 1}, {$set: {b: 1}}));
assert.eq(res.nMatched, 1, tojson(res));
assert.eq(res.nModified, 0, tojson(res));
assert.eq(res.nUpserted, 0, tojson(res));
res = assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
// Makes sure prepareTransaction returns prepareTimestamp in its response.
assert(res.hasOwnProperty("prepareTimestamp"), tojson(res));
assert.commandWorked(session.abortTransaction_forTesting());
