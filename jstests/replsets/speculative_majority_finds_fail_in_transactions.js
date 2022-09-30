/**
 * Verify that speculative majority find operations fail on replica sets with
 * 'enableMajorityReadConcern=false' when run inside of a transaction.
 * @tags: [uses_speculative_majority, requires_fcv_44]
 */
(function() {
"use strict";

const replTest = new ReplSetTest(
    {name: jsTestName(), nodes: 3, nodeOptions: {enableMajorityReadConcern: 'false'}});
replTest.startSet();
replTest.initiate();

const dbName = "speculative_majority_finds_fails_in_transactions";
const collName = "coll";
const primaryDB = replTest.getPrimary().getDB(dbName);
let session = primaryDB.getMongo().startSession();
let sessionDB = session.getDatabase(dbName);

// Issuing a regular majority find without the 'allowSpeculativeMajorityRead'
// flag should fail with a ReadConcernMajorityNotEnabled error.
session.startTransaction();
let res = sessionDB.runCommand({find: collName, readConcern: {level: "majority"}});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
session.endSession();

// Issuing a speculative majority find as the first transaction in a session should fail. Note
// that this operation will also fail with ReadConcernMajorityNotEnabled. This is because the
// first transaction on a session performs an internal find operation with the given read concern,
// and in this case that find is identical to the example above.
session = primaryDB.getMongo().startSession();
sessionDB = session.getDatabase(dbName);

session.startTransaction();
res = sessionDB.runCommand({
    find: collName,
    readConcern: {level: "majority"},
    allowSpeculativeMajorityRead: true,
});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
session.endSession();

// Any subsequent transactions that issue a speculative majority find should fail with an
// OperationNotSupportedInTransaction error.
session = primaryDB.getMongo().startSession();
sessionDB = session.getDatabase(dbName);

session.startTransaction();
assert.commandWorked(sessionDB.runCommand({find: collName}));
session.commitTransaction();

session.startTransaction();
res = sessionDB.runCommand({
    find: collName,
    readConcern: {level: "majority"},
    allowSpeculativeMajorityRead: true,
});
assert.commandFailedWithCode(res, ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

replTest.stopSet();
})();
