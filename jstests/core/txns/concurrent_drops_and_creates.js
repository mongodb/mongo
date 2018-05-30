// Test that a transaction cannot write to a collection that has been dropped or created since the
// transaction started.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName1 = "test1";
    const dbName2 = "test2";
    const collNameA = "coll_A";
    const collNameB = "coll_B";
    const testDB1 = db.getSiblingDB(dbName1);
    const testDB2 = db.getSiblingDB(dbName2);
    testDB1.runCommand({drop: collNameA, writeConcern: {w: "majority"}});
    testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}});

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB1 = session.getDatabase(dbName1);
    const sessionDB2 = session.getDatabase(dbName2);
    const sessionCollA = sessionDB1[collNameA];
    const sessionCollB = sessionDB2[collNameB];

    //
    // A transaction cannot write to a collection that has been dropped since the transaction
    // started.
    //

    // Ensure collection A and collection B exist.
    assert.commandWorked(sessionCollA.insert({}));
    assert.commandWorked(sessionCollB.insert({}));

    // Start the transaction with a write to collection A.
    session.startTransaction();
    assert.commandWorked(sessionCollA.insert({}));

    // Drop collection B outside of the transaction.
    assert.commandWorked(testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}}));

    // We cannot write to collection B in the transaction, since it is illegal to implicitly create
    // collections in transactions. The collection drop is visible to the transaction in this way,
    // since our implementation of the in-memory collection catalog always has the most recent
    // collection metadata.
    assert.commandFailedWithCode(sessionCollB.insert({}), ErrorCodes.NamespaceNotFound);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    //
    // A transaction cannot write to a collection that has been created since the transaction
    // started.
    //

    // Ensure collection A exists and collection B does not exist.
    assert.commandWorked(sessionCollA.insert({}));
    testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}});

    // Start the transaction with a write to collection A.
    session.startTransaction();
    assert.commandWorked(sessionCollA.insert({}));

    // Create collection B outside of the transaction.
    assert.commandWorked(testDB2.runCommand({create: collNameB}));

    // We cannot write to collection B in the transaction, since it experienced catalog changes
    // since the transaction's read timestamp. Since our implementation of the in-memory collection
    // catalog always has the most recent collection metadata, we do not allow you to read from a
    // collection at a time prior to its most recent catalog changes.
    assert.commandFailedWithCode(sessionCollB.insert({}), ErrorCodes.SnapshotUnavailable);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();
}());
