// Tests that reads and writes to the config and local databases are forbidden within
// transactions on non-sharded clusters. Behavior on sharded clusters is tested separately.
//
// @tags: [
//  # The test runs commands that are not allowed with security token: endSession.
//  not_allowed_with_signed_security_token,
//  assumes_against_mongod_not_mongos,
//  assumes_unsharded_collection,
//  uses_transactions,
//  # Transactions on config and local dbs are allowed on shardsvrs.
//  directly_against_shardsvrs_incompatible,
// ]

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

if (!TestData.testingReplicaSetEndpoint) {
    // This test drops a collection the config database, which is not allowed via a router on a
    // sharded cluster.
    runTest(session.getDatabase("config"));
}
runTest(session.getDatabase("local"));

session.endSession();
