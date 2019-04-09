/**
 * Tests that users with the 'internal' privilege can run the prepareTransaction command only.
 * The test also verifies that the prepareTransaction command can only be run against replica set
 * primaries.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 2, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();

    const adminDB = rst.getPrimary().getDB("admin");

    // Create the admin user.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert.eq(1, adminDB.auth("admin", "admin"));

    // Set up the test database.
    const dbName = "test";
    const collName = "transactions";
    const testDB = adminDB.getSiblingDB(dbName);
    testDB.dropDatabase();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Create two users. Alice will be given the 'internal' privilege.
    assert.commandWorked(
        adminDB.runCommand({createUser: "Alice", pwd: "pwd", roles: ["root", "__system"]}));
    assert.commandWorked(adminDB.runCommand({createUser: "Mallory", pwd: "pwd", roles: ["root"]}));
    adminDB.logout();

    /**
     * Test the prepareTransaction command with Alice who has the 'internal' privilege.
     */
    assert.eq(1, adminDB.auth("Alice", "pwd"));
    let lsid = assert.commandWorked(testDB.runCommand({startSession: 1})).id;

    // Start the transaction and insert a document.
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{_id: "alice"}],
        lsid: lsid,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));

    // Try to run prepareTransaction against the secondary.
    assert.commandFailedWithCode(rst.getSecondary().getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(1),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.Unauthorized);

    // Run prepareTransaction against the primary.
    const prepareTimestamp = assert
                                 .commandWorked(testDB.adminCommand({
                                     prepareTransaction: 1,
                                     lsid: lsid,
                                     txnNumber: NumberLong(0),
                                     stmtId: NumberInt(1),
                                     autocommit: false,
                                     writeConcern: {w: "majority"}
                                 }))
                                 .prepareTimestamp;
    const commitTimestamp = Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);

    // Commit the prepared transaction.
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: commitTimestamp,
        lsid: lsid,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(2),
        autocommit: false
    }));

    assert.eq(1, testDB[collName].find({_id: "alice"}).itcount());
    adminDB.logout();

    /**
     * Test the prepareTransaction command with Mallory who does not have the 'internal' privilege.
     */
    assert.eq(1, adminDB.auth("Mallory", "pwd"));

    // Start the transaction and insert a document.
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{_id: "mallory"}],
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));

    // Try to run prepareTransaction against the secondary.
    assert.commandFailedWithCode(rst.getSecondary().getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(1),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.Unauthorized);

    // Run prepareTransaction against the primary.
    assert.commandFailedWithCode(testDB.adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(1),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.Unauthorized);

    // Cannot commit the transaction with 'commitTimestamp'.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: Timestamp(0, 0),
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(1),
        autocommit: false
    }),
                                 ErrorCodes.InvalidOptions);

    // The transaction should be aborted.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(1),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(0, testDB[collName].find({_id: "mallory"}).itcount());
    adminDB.logout();

    /**
     * Test the prepareTransaction command with an unauthenticated user.
     */

    // Start the transaction and insert a document.
    assert.commandFailedWithCode(testDB.runCommand({
        insert: collName,
        documents: [{_id: "unauthenticated"}],
        lsid: lsid,
        txnNumber: NumberLong(2),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.Unauthorized);

    // Try to run prepareTransaction against the secondary.
    assert.commandFailedWithCode(rst.getSecondary().getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(2),
        stmtId: NumberInt(0),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.Unauthorized);

    // Run prepareTransaction against the primary.
    assert.commandFailedWithCode(testDB.adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(2),
        stmtId: NumberInt(0),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.Unauthorized);

    // Cannot commit the transaction.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: Timestamp(0, 0),
        lsid: lsid,
        txnNumber: NumberLong(2),
        stmtId: NumberInt(0),
        autocommit: false
    }),
                                 ErrorCodes.Unauthorized);

    assert.eq(1, adminDB.auth("Alice", "pwd"));
    assert.eq(0, testDB[collName].find({_id: "unauthenticated"}).itcount());
    assert.commandWorked(testDB.runCommand({endSessions: [lsid]}));
    adminDB.logout();

    rst.stopSet();
}());
