// Tests that reads and writes to the config and local databases are forbidden within
// transactions on non-sharded clusters. Behavior on sharded clusters is tested separately.
// The test runs commands that are not allowed with security token: endSession.
// @tags: [
//   not_allowed_with_security_token,
//  assumes_against_mongod_not_mongos,
//  assumes_unsharded_collection,
//  uses_transactions,
//  # Transactions on config and local dbs are allowed on shardsvrs.
//  # TODO SERVER-64544: Investigate if we should ban transactions on config and local db's in
//  # serverless. If yes, we will remove this tag.
//  directly_against_shardsvrs_incompatible,
// ]
(function() {
"use strict";

const session = db.getMongo().startSession({causalConsistency: false});
const collName = "banned_txn_dbs";

function runTest(sessionDB) {
    jsTest.log("Testing database " + sessionDB.getName());

    let sessionColl = sessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    jsTest.log("Testing read commands are forbidden.");
    session.startTransaction();
    let error = assert.throws(() => sessionColl.find().itcount());
    assert.commandFailedWithCode(error, ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("Testing write commands are forbidden.");
    session.startTransaction();
    assert.commandFailedWithCode(sessionColl.insert({}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}

runTest(session.getDatabase("config"));
runTest(session.getDatabase("local"));

session.endSession();
}());
