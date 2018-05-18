// Test that the read concern level 'snapshot' exhibits the correct yielding behavior. That is,
// operations performed at read concern level snapshot check for interrupt but do not yield locks or
// storage engine resources.
// @tags: [requires_replication]
(function() {
    "use strict";

    // Skip this test if running with --nojournal and WiredTiger.
    if (jsTest.options().noJournal &&
        (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
        print("Skipping test because running WiredTiger without journaling isn't a valid" +
              " replica set configuration");
        return;
    }

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(dbName);
    const adminDB = db.getSiblingDB("admin");
    const coll = db.coll;
    TestData.numDocs = 4;

    if (!db.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    // Increase the timeout for the transaction reaper. This will make the test easier to debug if
    // it hangs.
    // TODO SERVER-34595: This should no longer be necessary once the transaction reaper timeout
    // is increased for all noPassthrough tests.
    assert.commandWorked(
        db.adminCommand({"setParameter": 1, transactionLifetimeLimitSeconds: 60 * 60 * 3}));

    TestData.sessionId = assert.commandWorked(adminDB.runCommand({startSession: 1})).id;
    TestData.txnNumber = 0;

    // Set 'internalQueryExecYieldIterations' to 2 to ensure that commands yield on the second try
    // (i.e. after they have established a snapshot but before they have returned any documents).
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 2}));

    function waitForOpId(curOpFilter) {
        let opId;
        assert.soon(
            function() {
                const res = adminDB
                                .aggregate([
                                    {$currentOp: {}},
                                    {$match: {$and: [{ns: coll.getFullName()}, curOpFilter]}}
                                ])
                                .toArray();
                if (res.length === 1) {
                    opId = res[0].opid;
                    return true;
                }
                return false;
            },
            function() {
                return "Failed to find operation in $currentOp output: " +
                    tojson(adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName()}}])
                               .toArray());
            });
        return opId;
    }

    function assertKillPending(opId) {
        const res =
            adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName(), opid: opId}}])
                .toArray();
        assert.eq(res.length,
                  1,
                  tojson(adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName()}}])
                             .toArray()));
        assert(res[0].hasOwnProperty("killPending"), tojson(res));
        assert.eq(true, res[0].killPending, tojson(res));
    }

    function populateCollection() {
        db.coll.drop();
        for (let i = 0; i < TestData.numDocs; i++) {
            assert.commandWorked(
                db.coll.insert({_id: i, x: 1, location: [0, 0]}, {writeConcern: {w: "majority"}}));
        }

        assert.commandWorked(db.runCommand({
            createIndexes: "coll",
            indexes: [{key: {location: "2d"}, name: "geo_2d"}],
            writeConcern: {w: "majority"}
        }));
    }

    function testCommand(awaitCommandFn, curOpFilter, testWriteConflict) {
        //
        // Test that the command can be killed.
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        let awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);

        // Kill the command, and check that it is set to killPending.
        let opId = waitForOpId(curOpFilter);
        assert.commandWorked(db.killOp(opId));
        assertKillPending(opId);

        // Remove the hang, and check that the command is killed.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        let exitCode = awaitCommand({checkExitSuccess: false});
        assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

        //
        // Test that the command does not yield locks.
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
        waitForOpId(curOpFilter);

        // Start a drop. This should block behind the command, since the command does not yield
        // locks.
        let awaitDrop = startParallelShell(function() {
            db.getSiblingDB("test").coll.drop();
        }, rst.ports[0]);

        // Remove the hang. The command should complete successfully.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        awaitCommand();

        // Now the drop can complete.
        awaitDrop();

        //
        // Test that the command does not read data that is inserted during its execution.
        // 'awaitCommandFn' should fail if it reads the following document:
        //     {_id: <numDocs>, x: 1, new: 1, location: [0, 0]}
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
        waitForOpId(curOpFilter);

        // Insert data that should not be read by the command.
        assert.commandWorked(db.coll.insert({_id: TestData.numDocs, x: 1, new: 1, location: [0, 0]},
                                            {writeConcern: {w: "majority"}}));

        // Remove the hang. The command should complete successfully.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        awaitCommand();

        //
        // Test that the command fails if a write conflict occurs. 'awaitCommandFn' should write to
        // the following document: {_id: <numDocs>, x: 1, new: 1, location: [0, 0]}
        //

        if (testWriteConflict) {
            TestData.txnNumber++;
            populateCollection();

            // Insert the document that the command will write to.
            assert.commandWorked(
                db.coll.insert({_id: TestData.numDocs, x: 1, new: 1, location: [0, 0]},
                               {writeConcern: {w: "majority"}}));

            // Start a command that hangs before checking for interrupt.
            assert.commandWorked(db.adminCommand({
                configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang",
                mode: "alwaysOn"
            }));
            awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
            waitForOpId(curOpFilter);

            // Update the document that the command will write to.
            assert.commandWorked(db.coll.update({_id: TestData.numDocs},
                                                {$set: {conflict: true}},
                                                {writeConcern: {w: "majority"}}));

            // Remove the hang. The command should fail.
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
            exitCode = awaitCommand({checkExitSuccess: false});
            assert.neq(0, exitCode, "Expected shell to exit with failure due to WriteConflict");
        }
    }

    // Test find.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            find: "coll",
            filter: {x: 1},
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(res.cursor.firstBatch.length, TestData.numDocs, tojson(res));
    }, {"command.filter": {x: 1}});

    // Test getMore on a find established cursor.
    testCommand(function() {
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        const initialFindBatchSize = 2;
        const cursorId = assert
                             .commandWorked(db.runCommand({
                                 find: "coll",
                                 filter: {x: 1},
                                 batchSize: initialFindBatchSize,
                                 readConcern: {level: "snapshot"},
                                 lsid: TestData.sessionId,
                                 txnNumber: NumberLong(TestData.txnNumber)
                             }))
                             .cursor.id;
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        const res = assert.commandWorked(db.runCommand({
            getMore: NumberLong(cursorId),
            collection: "coll",
            batchSize: TestData.numDocs,
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(
            res.cursor.nextBatch.length, TestData.numDocs - initialFindBatchSize, tojson(res));
    }, {"originatingCommand.filter": {x: 1}});

    // Test aggregate.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            aggregate: "coll",
            pipeline: [{$match: {x: 1}}],
            readConcern: {level: "snapshot"},
            cursor: {},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(res.cursor.firstBatch.length, TestData.numDocs, tojson(res));
    }, {"command.pipeline": [{$match: {x: 1}}]});

    // TODO: SERVER-34113 Remove this test when we completely remove snapshot
    // reads since this command is not supported with transaction api.
    // Test geoNear.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            geoNear: "coll",
            near: [0, 0],
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert(res.hasOwnProperty("results"));
        assert.eq(res.results.length, TestData.numDocs, tojson(res));
    }, {"command.geoNear": "coll"});

    // Test getMore with an initial find batchSize of 0. Interrupt behavior of a getMore is not
    // expected to change with a change of batchSize in the originating command.
    testCommand(function() {
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        const initialFindBatchSize = 0;
        const cursorId = assert
                             .commandWorked(db.runCommand({
                                 find: "coll",
                                 filter: {x: 1},
                                 batchSize: initialFindBatchSize,
                                 readConcern: {level: "snapshot"},
                                 lsid: TestData.sessionId,
                                 txnNumber: NumberLong(TestData.txnNumber)
                             }))
                             .cursor.id;
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        const res = assert.commandWorked(db.runCommand({
            getMore: NumberLong(cursorId),
            collection: "coll",
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(
            res.cursor.nextBatch.length, TestData.numDocs - initialFindBatchSize, tojson(res));
    }, {"originatingCommand.filter": {x: 1}});

    // Test count.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            count: "coll",
            query: {_id: {$ne: 0}},
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(res.n, 3, tojson(res));
    }, {"command.count": "coll"});

    // Test distinct.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            distinct: "coll",
            key: "_id",
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert(res.hasOwnProperty("values"));
        assert.eq(res.values.length, 4, tojson(res));
    }, {"command.distinct": "coll"});

    // Test group.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            group: {ns: "coll", key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}},
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert(res.hasOwnProperty("count"), tojson(res));
        assert.eq(res.count, 4);
    }, {"command.group.ns": "coll"});

    // Test update.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            update: "coll",
            updates:
                [{q: {}, u: {$set: {updated: true}}}, {q: {new: 1}, u: {$set: {updated: true}}}],
            readConcern: {level: "snapshot"},
            startTransaction: true,
            autocommit: false,
            stmtId: NumberInt(0),
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.commandWorked(db.adminCommand({
            commitTransaction: 1,
            autocommit: false,
            lsid: TestData.sessionId,
            stmtId: NumberInt(1),
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        // Only update one existing doc committed before the transaction.
        assert.eq(res.n, 1, tojson(res));
        assert.eq(res.nModified, 1, tojson(res));
    }, {op: "update"}, true);

    // Test delete.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            delete: "coll",
            deletes: [{q: {}, limit: 1}, {q: {new: 1}, limit: 1}],
            readConcern: {level: "snapshot"},
            startTransaction: true,
            autocommit: false,
            txnNumber: NumberLong(TestData.txnNumber),
            stmtId: NumberInt(0),
            lsid: TestData.sessionId
        }));
        assert.commandWorked(db.adminCommand({
            commitTransaction: 1,
            autocommit: false,
            lsid: TestData.sessionId,
            stmtId: NumberInt(1),
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        // Only remove one existing doc committed before the transaction.
        assert.eq(res.n, 1, tojson(res));
    }, {op: "remove"}, true);

    // Test findAndModify.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            findAndModify: "coll",
            query: {new: 1},
            update: {$set: {findAndModify: 1}},
            readConcern: {level: "snapshot"},
            startTransaction: true,
            autocommit: false,
            txnNumber: NumberLong(TestData.txnNumber),
            stmtId: NumberInt(0),
            lsid: TestData.sessionId,
        }));
        assert.commandWorked(db.adminCommand({
            commitTransaction: 1,
            autocommit: false,
            lsid: TestData.sessionId,
            stmtId: NumberInt(1),
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert(res.hasOwnProperty("lastErrorObject"));
        assert.eq(res.lastErrorObject.n, 0, tojson(res));
        assert.eq(res.lastErrorObject.updatedExisting, false, tojson(res));
    }, {"command.findAndModify": "coll"}, true);

    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            findAndModify: "coll",
            query: {new: 1},
            update: {$set: {findAndModify: 1}},
            readConcern: {level: "snapshot"},
            startTransaction: true,
            autocommit: false,
            txnNumber: NumberLong(TestData.txnNumber),
            stmtId: NumberInt(0),
            lsid: TestData.sessionId,
        }));
        assert.commandWorked(db.adminCommand({
            commitTransaction: 1,
            autocommit: false,
            lsid: TestData.sessionId,
            stmtId: NumberInt(1),
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert(res.hasOwnProperty("lastErrorObject"));
        assert.eq(res.lastErrorObject.n, 0, tojson(res));
        assert.eq(res.lastErrorObject.updatedExisting, false, tojson(res));
    }, {"command.findAndModify": "coll"}, true);

    rst.stopSet();
}());
