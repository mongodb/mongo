/**
 * Test that transaction operations wait for write concern (or don't) correctly on noop writes.
 *
 * We run most commands on a different connection. If the commands were run on the same
 * connection, then the client last op for the noop writes would be set by the previous operation.
 * By using a fresh connection the client last op begins as null.  This test explicitly tests that
 * write concern for noop writes works when the client last op has not already been set by a
 * duplicate operation.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collNameBase = "coll";

    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const failTimeoutMS = 1000;
    const successTimeoutMS = ReplSetTest.kDefaultTimeoutMS;

    function runTest(readConcernLevel) {
        jsTestLog("Testing " + readConcernLevel);

        const collName = `${collNameBase}_${readConcernLevel}`;
        assert.commandWorked(primaryDB[collName].insert(
            [{x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}, {x: 6}], {writeConcern: {w: "majority"}}));

        jsTestLog("Unprepared Abort Setup");
        const mongo1 = new Mongo(primary.host);
        const session1 = mongo1.startSession();
        const sessionDB1 = session1.getDatabase(dbName);
        session1.startTransaction({
            writeConcern: {w: "majority", wtimeout: successTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate1 = {update: collName, updates: [{q: {x: 1}, u: {$set: {x: 1}}}]};
        printjson(assert.commandWorked(sessionDB1.runCommand(fruitlessUpdate1)));

        jsTestLog("Prepared Abort Setup");
        const mongo2 = new Mongo(primary.host);
        const session2 = mongo2.startSession();
        const sessionDB2 = session2.getDatabase(dbName);
        session2.startTransaction({
            writeConcern: {w: "majority", wtimeout: failTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate2 = {update: collName, updates: [{q: {x: 2}, u: {$set: {x: 2}}}]};
        printjson(assert.commandWorked(sessionDB2.runCommand(fruitlessUpdate2)));
        PrepareHelpers.prepareTransaction(session2);

        jsTestLog("Prepare Setup");
        const mongo3 = new Mongo(primary.host);
        const session3 = mongo3.startSession();
        const sessionDB3 = session3.getDatabase(dbName);
        session3.startTransaction({
            writeConcern: {w: "majority", wtimeout: failTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate3 = {update: collName, updates: [{q: {x: 3}, u: {$set: {x: 3}}}]};
        printjson(assert.commandWorked(sessionDB3.runCommand(fruitlessUpdate3)));

        jsTestLog("Unprepared Commit Setup");
        const mongo4 = new Mongo(primary.host);
        const session4 = mongo4.startSession();
        const sessionDB4 = session4.getDatabase(dbName);
        session4.startTransaction({
            writeConcern: {w: "majority", wtimeout: failTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate4 = {update: collName, updates: [{q: {x: 4}, u: {$set: {x: 4}}}]};
        printjson(assert.commandWorked(sessionDB4.runCommand(fruitlessUpdate4)));

        jsTestLog("Prepared Commit Setup");
        const mongo5 = new Mongo(primary.host);
        const session5 = mongo5.startSession();
        const sessionDB5 = session5.getDatabase(dbName);
        session5.startTransaction({
            writeConcern: {w: "majority", wtimeout: failTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate5 = {update: collName, updates: [{q: {x: 5}, u: {$set: {x: 5}}}]};
        printjson(assert.commandWorked(sessionDB5.runCommand(fruitlessUpdate5)));
        let prepareTS5 = PrepareHelpers.prepareTransaction(session5);

        jsTestLog("Unprepared Abort On Used Connection Setup");
        const session6 = primary.getDB("admin").getMongo().startSession();
        const sessionDB6 = session6.getDatabase(dbName);
        session6.startTransaction({
            writeConcern: {w: "majority", wtimeout: failTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        const fruitlessUpdate6 = {update: collName, updates: [{q: {x: 6}, u: {$set: {x: 6}}}]};
        printjson(assert.commandWorked(sessionDB6.runCommand(fruitlessUpdate6)));

        jsTestLog("Stop replication");
        stopReplicationOnSecondaries(rst);

        jsTestLog("Advance OpTime on primary, with replication stopped");

        printjson(assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{}]})));

        jsTestLog("Run test commands, with replication stopped");

        jsTestLog("Unprepared Abort Test");
        assert.commandWorked(session1.abortTransaction_forTesting());

        jsTestLog("Prepared Abort Test");
        assert.commandFailedWithCode(session2.abortTransaction_forTesting(),
                                     ErrorCodes.WriteConcernFailed);

        jsTestLog("Prepare Test");
        assert.commandFailedWithCode(
            session3.getDatabase('admin').adminCommand(
                {prepareTransaction: 1, writeConcern: {w: "majority", wtimeout: failTimeoutMS}}),
            ErrorCodes.WriteConcernFailed);
        assert.commandFailedWithCode(session3.abortTransaction_forTesting(),
                                     ErrorCodes.WriteConcernFailed);

        jsTestLog("Unprepared Commit Test");
        assert.commandFailedWithCode(session4.commitTransaction_forTesting(),
                                     ErrorCodes.WriteConcernFailed);

        jsTestLog("Prepared Commit Test");
        assert.commandFailedWithCode(session5.getDatabase('admin').adminCommand({
            commitTransaction: 1,
            commitTimestamp: prepareTS5,
            writeConcern: {w: "majority", wtimeout: failTimeoutMS}
        }),
                                     ErrorCodes.WriteConcernFailed);
        // Send commit with the shell helper to reset the shell's state.
        assert.commandFailedWithCode(session5.commitTransaction_forTesting(),
                                     ErrorCodes.WriteConcernFailed);

        jsTestLog("Unprepared Abort On Used Connection Test");
        assert.commandFailedWithCode(session6.abortTransaction_forTesting(),
                                     ErrorCodes.WriteConcernFailed);

        jsTestLog("Restart replication");
        restartReplicationOnSecondaries(rst);

        jsTestLog("Try transaction with replication enabled");

        // Unprepared Abort.
        session1.startTransaction({
            writeConcern: {w: "majority", wtimeout: successTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        assert.commandWorked(sessionDB1.runCommand(fruitlessUpdate1));
        assert.commandWorked(session1.abortTransaction_forTesting());

        // Prepared Abort.
        session2.startTransaction({
            writeConcern: {w: "majority", wtimeout: successTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        assert.commandWorked(sessionDB2.runCommand(fruitlessUpdate2));
        PrepareHelpers.prepareTransaction(session2);
        assert.commandWorked(session2.abortTransaction_forTesting());

        // Testing prepare is no different then prepared abort or prepared commit.

        // Unprepared Commit.
        session4.startTransaction({
            writeConcern: {w: "majority", wtimeout: successTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        assert.commandWorked(sessionDB4.runCommand(fruitlessUpdate4));
        assert.commandWorked(session4.commitTransaction_forTesting());

        // Prepared Commit.
        session5.startTransaction({
            writeConcern: {w: "majority", wtimeout: successTimeoutMS},
            readConcern: {level: readConcernLevel}
        });
        assert.commandWorked(sessionDB5.runCommand(fruitlessUpdate5));
        prepareTS5 = PrepareHelpers.prepareTransaction(session5);
        assert.commandWorked(session5.getDatabase('admin').adminCommand({
            commitTransaction: 1,
            commitTimestamp: prepareTS5,
            writeConcern: {w: "majority", wtimeout: successTimeoutMS}
        }));
        // Send commit with the shell helper to reset the shell's state.
        assert.commandWorked(session5.commitTransaction_forTesting());

        // Unprepared abort already is using a "used connection" for this success test.
    }

    runTest("local");
    runTest("majority");
    runTest("snapshot");

    rst.stopSet();
}());
