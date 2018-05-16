// Test commands that are not allowed in multi-document transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "commands_not_allowed_in_txn";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [
            {name: "geo_2d", key: {geo: "2d"}},
            {key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}
        ],
        writeConcern: {w: "majority"}
    }));

    function setup() {
        testColl.dropIndex({a: 1});
        testDB.runCommand({drop: "create_collection", writeConcern: {w: "majority"}});
        testDB.runCommand({drop: "drop_collection", writeConcern: {w: "majority"}});
        assert.commandWorked(
            testDB.createCollection("drop_collection", {writeConcern: {w: "majority"}}));
    }

    function testCommand(command) {
        jsTest.log("Testing command: " + tojson(command));

        // Check that the command runs successfully outside transactions.
        setup();
        assert.commandWorked(sessionDb.runCommand(command));

        // Check that the command cannot be used to start a transaction.
        setup();
        assert.commandFailedWithCode(sessionDb.runCommand(Object.assign({}, command, {
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false
        })),
                                     [50767, 50768]);
        assert.commandFailedWithCode(sessionDb.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }),
                                     ErrorCodes.NoSuchTransaction);

        // Check that the command fails inside a transaction, but does not abort the transaction.
        setup();
        assert.commandWorked(sessionDb.runCommand({
            insert: collName,
            documents: [{}],
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false
        }));
        assert.commandFailedWithCode(
            sessionDb.runCommand(Object.assign(
                {},
                command,
                {txnNumber: NumberLong(txnNumber), stmtId: NumberInt(1), autocommit: false})),
            [50767, 50768]);
        assert.commandWorked(sessionDb.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(2),
            autocommit: false
        }));
    }

    //
    // Test commands that check out the session but are not allowed in multi-document
    // transactions.
    //

    const sessionCommands = [
        {applyOps: [{op: "u", ns: testColl.getFullName(), o2: {_id: 0}, o: {$set: {a: 5}}}]},
        {explain: {find: collName}},
        {eval: "function() {return 1;}"},
        {"$eval": "function() {return 1;}"},
        {filemd5: 1, root: "fs"},
        {geoNear: collName, near: [0, 0]},
        {group: {ns: collName, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}}},
        {mapReduce: collName, map: function() {}, reduce: function(key, vals) {}, out: "out"},
        {parallelCollectionScan: collName, numCursors: 1},
    ];

    sessionCommands.forEach(testCommand);

    //
    // Test a selection of commands that do not check out the session. It is illegal to provide a
    // 'txnNumber' on these commands.
    //

    const nonSessionCommands = [
        {create: "create_collection", writeConcern: {w: "majority"}},
        {drop: "drop_collection", writeConcern: {w: "majority"}},
        {
          createIndexes: collName,
          indexes: [{name: "a_1", key: {a: 1}}],
          writeConcern: {w: "majority"}
        }
    ];

    nonSessionCommands.forEach(testCommand);

    nonSessionCommands.forEach(function(command) {
        setup();
        assert.commandFailedWithCode(
            sessionDb.runCommand(Object.assign({}, command, {txnNumber: NumberLong(++txnNumber)})),
            50768);
    });

    //
    // Test that doTxn is not allowed at positions after the first in transactions.
    //

    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandFailedWithCode(sessionDb.runCommand({
        doTxn: [{op: "u", ns: testColl.getFullName(), o2: {_id: 0}, o: {$set: {a: 5}}}],
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(1),
        autocommit: false
    }),
                                 ErrorCodes.ConflictingOperationInProgress);

    // It is still possible to commit the transaction. The rejected command does not abort the
    // transaction.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(2),
        autocommit: false
    }));

    session.endSession();
}());
