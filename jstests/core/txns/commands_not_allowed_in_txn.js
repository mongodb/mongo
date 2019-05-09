// Test commands that are not allowed in multi-document transactions.
// @tags: [uses_transactions, uses_snapshot_read_concern]
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

    const isMongos = assert.commandWorked(db.runCommand("ismaster")).msg === "isdbgrid";

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
        const errmsgRegExp = new RegExp(
            'Cannot run .* in a multi-document transaction.\|This command is not supported in transactions');

        // Check that the command runs successfully outside transactions.
        setup();
        assert.commandWorked(sessionDb.runCommand(command));

        // Check that the command cannot be used to start a transaction.
        setup();
        let res = assert.commandFailedWithCode(sessionDb.runCommand(Object.assign({}, command, {
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false
        })),
                                               ErrorCodes.OperationNotSupportedInTransaction);
        // Check that the command fails with expected error message.
        assert(res.errmsg.match(errmsgRegExp), res);

        // Mongos has special handling for commitTransaction to support commit recovery.
        if (!isMongos) {
            assert.commandFailedWithCode(sessionDb.adminCommand({
                commitTransaction: 1,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(1),
                autocommit: false
            }),
                                         ErrorCodes.NoSuchTransaction);
        }

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
        res = assert.commandFailedWithCode(
            sessionDb.runCommand(Object.assign(
                {},
                command,
                {txnNumber: NumberLong(txnNumber), stmtId: NumberInt(1), autocommit: false})),
            ErrorCodes.OperationNotSupportedInTransaction);
        // Check that the command fails with expected error message.
        assert(res.errmsg.match(errmsgRegExp), res);
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
        {count: collName},
        {count: collName, query: {a: 1}},
        {explain: {find: collName}},
        {filemd5: 1, root: "fs"},
    ];

    // There is no applyOps command on mongos.
    if (!isMongos) {
        sessionCommands.push(
            {applyOps: [{op: "u", ns: testColl.getFullName(), o2: {_id: 0}, o: {$set: {a: 5}}}]});
    }

    sessionCommands.forEach(testCommand);

    //
    // Test a selection of commands that do not check out the session. It is illegal to provide a
    // 'txnNumber' on these commands.
    //

    const nonSessionCommands = [
        {isMaster: 1},
        {buildInfo: 1},
        {ping: 1},
        {listCommands: 1},
        {create: "create_collection", writeConcern: {w: "majority"}},
        {drop: "drop_collection", writeConcern: {w: "majority"}},
        {
          createIndexes: collName,
          indexes: [{name: "a_1", key: {a: 1}}],
          writeConcern: {w: "majority"}
        },
        // Output inline so the implicitly shard accessed collections override won't drop the
        // output collection during the active transaction test case, which would hang indefinitely
        // waiting for a database exclusive lock.
        {mapReduce: collName, map: function() {}, reduce: function(key, vals) {}, out: {inline: 1}},
    ];

    nonSessionCommands.forEach(testCommand);

    nonSessionCommands.forEach(function(command) {
        setup();
        assert.commandFailedWithCode(
            sessionDb.runCommand(Object.assign({}, command, {txnNumber: NumberLong(++txnNumber)})),
            [50768, 50889]);
    });

    //
    // Test that doTxn is not allowed at positions after the first in transactions.
    //

    // There is no doTxn command on mongos.
    if (!isMongos) {
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
                                     ErrorCodes.OperationNotSupportedInTransaction);

        // It is still possible to commit the transaction. The rejected command does not abort the
        // transaction.
        assert.commandWorked(sessionDb.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(2),
            autocommit: false
        }));
    }

    //
    // Test that a find command with the read-once cursor option is not allowed in a transaction.
    //
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readOnce: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.OperationNotSupportedInTransaction);

    // Mongos has special handling for commitTransaction to support commit recovery.
    if (!isMongos) {
        // The failed find should abort the transaction so a commit should fail.
        assert.commandFailedWithCode(sessionDb.adminCommand({
            commitTransaction: 1,
            autocommit: false,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
        }),
                                     ErrorCodes.NoSuchTransaction);
    }

    session.endSession();
}());
