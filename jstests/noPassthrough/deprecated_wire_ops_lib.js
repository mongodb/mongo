/**
 * Helper routines for deprecated_wire_ops_mongod.js and deprecated_wire_ops_mongos.js.
 */
const DeprecatedWireOpsTest = function() {
    "use strict";

    if (!(this instanceof DeprecatedWireOpsTest)) {
        return new DeprecatedWireOpsTest();
    }

    // Warning code for deprecated op codes or getLastError command in log.
    const kDeprecatedWireOpWarningCode = 5578800;

    /**
     * Extracts deprecation warning messages from server log. Returns the extracted logging
     * message(s).
     *
     * If 'assertOpKindMatch' is true, asserts that all log entries' operations are same as
     * 'expectedOpKind'.
     */
    const extractDeprecationMsgFromLog =
        (testDB, connId, expectedOpKind, assertOpKindMatch = false) => {
            const log = assert.commandWorked(testDB.adminCommand({"getLog": "global"})).log;
            const connIdStr = `conn${connId}`;
            let logEntries = [];

            log.forEach((line) => {
                if (line.indexOf(`"id":${kDeprecatedWireOpWarningCode}`) >= 0) {
                    const logEntry = JSON.parse(line);

                    assert.eq(logEntry.ctx, connIdStr);
                    assert.eq(logEntry.id, kDeprecatedWireOpWarningCode);
                    assert.eq(logEntry.s, "W");
                    assert.eq(logEntry.c, "COMMAND");
                    if (logEntry.attr.op == expectedOpKind) {
                        logEntries.push(logEntry);
                    } else {
                        assert.eq(assertOpKindMatch,
                                  false,
                                  `Expected ${expectedOpKind} only but found ${logEntry.attr.op}`);
                    }
                }
            });

            return logEntries;
        };

    /**
     * Asserts that the warning message for legacy op codes and getLastError command is logged only
     * once for 'expectedOpKind'.
     */
    const assertLegacyOpWarnedOnlyOnceOn = (testDB, connId, expectedOpKind) => {
        const logEntries = extractDeprecationMsgFromLog(
            testDB, connId, expectedOpKind, /*assertOpKindMatch*/ true);

        // Verifies that the legacy op is logged with a warning message.
        assert.eq(logEntries.length, 1);
    };

    /**
     * Asserts that the number of log entries for warning messages for 'expectedOpKind' is same as
     * 'expectedCount' and all log entries are triggered under 'connId' connection.
     */
    const assertLegacyOpWarningMsgCountOn = (testDB, connId, expectedOpKind, expectedCount) => {
        const logEntries = extractDeprecationMsgFromLog(testDB, connId, expectedOpKind);

        // Verifies that the legacy op is logged with a warning message.
        assert.eq(logEntries.length, expectedCount);
    };

    /**
     * Sleeps for 'ms' milliseconds with logging messages.
     */
    const sleepWithLogging = (ms) => {
        jsTestLog(new Date().toString() + `: sleeping for ${ms}ms...`);
        sleep(ms);
        jsTestLog(new Date().toString() + ": woke up");
    };

    /**
     * Gets a legacy cursor on 'coll' collection so that we can send OP_GET_MORE or
     * OP_KILL_CURSORS. This can be done as follows.
     * 1. Sends a OP_MSG find command with a small batch size to create a cursor.
     * 2. Gets a legacy cursor object using cursorFromId() method which always uses OP_GET_MORE op
     *    code when hasNext() or next() is called, and OP_KILL_CURSORS op code when close() is
     *    called.
     */
    const getLegacyCursor = (db, coll) => {
        const findRes = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 1}));
        // cursor.id == 0 means the cursor reached the end of data, which must not happen.
        assert.neq(findRes.cursor.id, 0);
        return db.getMongo().cursorFromId(findRes.cursor.ns, findRes.cursor.id, /*batchSize*/ 2);
    };

    /**
     * Runs a basic test case for log warning behavior for legacy op codes or getLastError command.
     * The basic behavior is that only one warning message must be logged for the first legacy op
     * or getLastError command in a period for multiple legacy op codes and/or getLastError command
     * requests per each client.
     *
     * - 'legacyOpAction1': The first action to execute legacy ops or commands. This action is
     *   executed twice.
     * - 'legacyOpAction2': The second action to execute legacy ops or commands. This action is
     *   executed only once.
     * - 'verifyAction': An action to verify the results. This action is executed after every
     *   'legacyOpAction1' or 'legacyOpAction2'.
     */
    const runBasicLoggingBehaviorTest = (legacyOpAction1, legacyOpAction2, verifyAction) => {
        legacyOpAction1();
        verifyAction();

        legacyOpAction1();
        verifyAction();

        legacyOpAction2();
        verifyAction();
    };

    const nop = () => {};

    /**
     * - 'testCaseSetUp': An action to set up the test case.
     * - 'legacyOpAction': An action to execute legacy ops or commands. This is executed twice.
     * - 'verifyAction': An action to verify the results. This is executed after every
     *   'legacyOpAction'.
     * - 'sleepAction': An action on how to sleep. 'runLogAllDeprecatedWireOpsTest' sets this
     *   argument to 'nop' to verify the log-all behavior because the log-all behavior is only
     *   valid when period = 0 and when period = 0, we should not sleep at all.
     */
    const runPeriodTestCase = (testCaseSetUp, legacyOpAction, verifyAction, sleepAction = nop) => {
        testCaseSetUp();

        legacyOpAction();
        verifyAction();

        sleepAction();

        legacyOpAction();
        verifyAction();
    };

    /**
     * Gets a setParameter document for deprecation warning period 'periodInSeconds'.
     * The returned doc will be used to start up mongod or mongos.
     */
    this.getParamDoc = (periodInSeconds) => {
        return {setParameter: `deprecatedWireOpsWarningPeriodInSeconds=${periodInSeconds}`};
    };

    /**
     * Sets up the test environment and returns a connection 'conn', a test db 'testDB', a
     * connection id 'connId', and a collection 'coll'.
     *
     * 'startUp' is supposed to start a mongod or sharded cluster and returns a connection to it and
     * a test db.
     *
     * 'conn' is used to tear down the server, which means it's an input argument for tear down
     * callback.
     */
    this.setUp = (startUp, periodInSeconds) => {
        const [conn, testDB] = startUp(periodInSeconds);

        // Figures out the connection ID.
        const connId = assert.commandWorked(testDB.runCommand("hello")).connectionId;

        const coll = testDB.getCollection("a");
        coll.drop();

        // Prepares some data for OP_QUERY, OP_GET_MORE, and OP_KILL_CURSORS.
        assert.commandWorked(coll.insert({a: 0}));
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.insert({a: 2}));

        // Need to set the legacy read and write mode to issue deprecated op codes or command.
        const mongo = testDB.getMongo();
        mongo.forceReadMode("legacy");
        mongo.forceWriteMode("legacy");

        return [conn, testDB, connId, coll];
    };

    /**
     * Verifies the basic logging behavior for legacy op codes and getLastError command which is to
     * log a warning messages for the first legacy op code or getLastError command in 60-minutes
     * period per each client.
     *
     * 'setUp' is used to set up a test environment such as starting up mongod or a sharded cluster
     * and getting a test database object and etc. 'tearDown' is used to tear down the test
     * environment which is started by 'setUp'.
     */
    this.runDeprecatedWireOpBasicLoggingBehaviorTest = (setUp, tearDown) => {
        const assertLegacyOpWarnedOnlyOnceForOpKind = (expectedOpKind) =>
            assertLegacyOpWarnedOnlyOnceOn(testDB, connId, expectedOpKind);

        // Verifies that the server logs a warning message only once for two OP_INSERTs and one
        // OP_QUERY.
        let [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(() => coll.insert({a: 2, b: 3}),
                                    () => coll.find().toArray(),
                                    () => assertLegacyOpWarnedOnlyOnceForOpKind("insert"));
        tearDown(conn);

        // Verifies that the server logs a warning message only once for two OP_UPDATEs and one
        // OP_QUERY.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(() => coll.updateOne({a: 3}, {$set: {b: -1}}),
                                    () => coll.find().toArray(),
                                    () => assertLegacyOpWarnedOnlyOnceForOpKind("update"));
        tearDown(conn);

        // Verifies that the server logs a warning message only once for two OP_DELETEs and one
        // OP_QUERY.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(() => coll.deleteOne({a: 3}),
                                    () => coll.find().toArray(),
                                    () => assertLegacyOpWarnedOnlyOnceForOpKind("remove"));
        tearDown(conn);

        // Verifies that the server logs a warning message for two getLastError commands and one
        // OP_QUERY.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(
            () => assert.commandWorked(testDB.adminCommand({getLastError: 1})),
            () => coll.find().toArray(),
            () => assertLegacyOpWarnedOnlyOnceForOpKind("getLastError"));
        tearDown(conn);

        // Verifies that the server logs a warning message only once for two OP_QUERYs and one
        // OP_INSERT.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(() => assert.eq(coll.find().toArray().length, 3),
                                    () => coll.insert({a: 4}),
                                    () => assertLegacyOpWarnedOnlyOnceForOpKind("query"));
        tearDown(conn);

        // Verifies that the server logs a warning message only once for two OP_GET_MOREs and one
        // OP_UPDATE.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(
            () => {
                let legacyCursor = getLegacyCursor(testDB, coll);
                assert.eq(legacyCursor.hasNext(), true);
            },
            () => coll.updateOne({a: 1}, {$set: {b: -1}}),
            () => assertLegacyOpWarnedOnlyOnceForOpKind("getmore"));
        tearDown(conn);

        // Verifies that the server logs a warning message only once for two OP_KILL_CURSORS and one
        // OP_DELETE.
        [conn, testDB, connId, coll] = setUp();
        runBasicLoggingBehaviorTest(
            () => {
                let legacyCursor = getLegacyCursor(testDB, coll);
                legacyCursor.close();
            },
            () => coll.deleteOne({a: 1}),
            () => assertLegacyOpWarnedOnlyOnceForOpKind("killcursors"));
        tearDown(conn);
    };

    /**
     * Verifies that every legacy op code and getLastError command request is logged with a warning
     * message when the warning period is set to 0 seconds.
     *
     * 'setUp' is used to set up a test environment such as starting up mongod or a sharded cluster
     * and getting a test database object and etc. 'tearDown' is used to tear down the test
     * environment which is started by 'setUp'.
     */
    this.runLogAllDeprecatedWireOpsTest = (setUp, tearDown) => {
        const [conn, testDB, connId, coll] = setUp();
        const assertLegacyOpWarningMsgCount = (expectedOpKind, expectedCount) =>
            assertLegacyOpWarningMsgCountOn(testDB, connId, expectedOpKind, expectedCount);

        let nLegacyOpMsgExpected = 0;

        // Verifies that the server logs a warning message for every OP_INSERT.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
            },
            /*legacyOpAction*/
            () => {
                coll.insert({a: 1, b: 2});
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("insert", nLegacyOpMsgExpected));

        // Verifies that the server logs a warning message for every OP_UPDATE.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
            },
            /*legacyOpAction*/
            () => {
                coll.updateOne({a: 1}, {$set: {b: -1}});
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("update", nLegacyOpMsgExpected));

        // Verifies that the server logs a warning message for every OP_DELETE.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
            },
            /*legacyOpAction*/
            () => {
                coll.deleteOne({a: 1});
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("remove", nLegacyOpMsgExpected));

        // Verifies that the server logs a warning message for every getLastError.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                // First figure out how many getLastError commands have already been issued since
                // we're in "legacy" write mode.
                nLegacyOpMsgExpected =
                    extractDeprecationMsgFromLog(testDB,
                                                 connId,
                                                 /*expectedOpKind*/ "getLastError")
                        .length;
            },
            /*legacyOpAction*/
            () => {
                assert.commandWorked(testDB.adminCommand({getLastError: 1}));
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("getLastError", nLegacyOpMsgExpected));

        // Verifies that the server logs a warning message for every OP_QUERY.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
                coll.drop();
                testDB.getMongo().forceWriteMode("commands");
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.insert({a: 2}));
                assert.commandWorked(coll.insert({a: 3}));
                testDB.getMongo().forceWriteMode("legacy");
            },
            /*legacyOpAction*/
            () => {
                const data = coll.find().toArray();
                assert.eq(data.length, 3);
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("query", nLegacyOpMsgExpected));

        // Verifies that the server logs a warning message for every OP_GET_MORE & OP_KILL_CURSORS.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
            },
            /*legacyOpAction*/
            () => {
                const legacyCursor = getLegacyCursor(testDB, coll);
                legacyCursor.hasNext();
                legacyCursor.close();
                ++nLegacyOpMsgExpected;
            },
            /*verifyAction*/
            () => {
                assertLegacyOpWarningMsgCount("getmore", nLegacyOpMsgExpected);
                assertLegacyOpWarningMsgCount("killcursors", nLegacyOpMsgExpected);
            });

        tearDown(conn);
    };

    /**
     * Verifies that the warning period value is honored. The focus here is to verify that after the
     * period expires, any legacy op code or getLastError command is logged with a warning message.
     *
     * 'setUp' is used to set up a test environment such as starting up mongod or a sharded cluster
     * and getting a test database object and etc. 'tearDown' is used to tear down the test
     * environment which is started by 'setUp'.
     */
    this.runDeprecatedWireOpPeriodTest = (setUp, tearDown, periodInSeconds) => {
        const [conn, testDB, connId, coll] = setUp(periodInSeconds);
        const periodInMs = periodInSeconds * 1000 + 200;
        const assertLegacyOpWarningMsgCount = (expectedOpKind, expectedCount) =>
            assertLegacyOpWarningMsgCountOn(testDB, connId, expectedOpKind, expectedCount);

        let nLegacyOpMsgExpected = 0;

        // Verifies that the server logs a warning message only once in 'periodInSeconds' for
        // multiple OP_INSERTs.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
            },
            /*legacyOpAction*/
            () => {
                // This test case is timing-sensitive. Can't wait for votes and journaling.
                const noVotesNoJournalingWc = {w: 0, j: false};
                coll.insert({a: 1, b: 2}, noVotesNoJournalingWc);
                ++nLegacyOpMsgExpected;
                coll.insert({a: 1, b: 2}, noVotesNoJournalingWc);
                coll.insert({a: 1, b: 2}, noVotesNoJournalingWc);
                coll.insert({a: 1, b: 2}, noVotesNoJournalingWc);
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("insert", nLegacyOpMsgExpected),
            /*sleepAction*/
            () => sleepWithLogging(periodInMs));

        // Verifies that the server logs a warning message only once in 'periodInSeconds' for
        // multiple OP_QUERYs.
        runPeriodTestCase(
            /*testCaseSetUp*/
            () => {
                nLegacyOpMsgExpected = 0;
                coll.drop();
                testDB.getMongo().forceWriteMode("commands");
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.insert({a: 1}));
                testDB.getMongo().forceWriteMode("legacy");

                // Sleeps to make sure the previous period expires.
                sleepWithLogging(periodInMs);
            },
            /*legacyOpAction*/
            () => {
                assert.eq(coll.find().toArray().length, 2);
                ++nLegacyOpMsgExpected;
                assert.eq(coll.find().toArray().length, 2);
                assert.eq(coll.find().toArray().length, 2);
            },
            /*verifyAction*/
            () => assertLegacyOpWarningMsgCount("query", nLegacyOpMsgExpected),
            /*sleepAction*/
            () => sleepWithLogging(periodInMs));

        tearDown(conn);
    };

    this.runDeprecatedOpcountersInServerStatusTest = (setUp, tearDown) => {
        const [conn, dbTest] = setUp();

        const insertCount = 5;
        const batchSize = 3;

        function exerciseOperations(collection) {
            // Insert (each document is counted as separate insert op)
            let docs = [];
            for (let i = 0; i < insertCount; i++) {
                docs.push({a: i});
            }
            assert.commandWorked(collection.insertMany(docs));

            // Query and getmore (one query op, count of getmore ops depends on ratio of insertCount
            // to batchSize)
            collection.find().batchSize(batchSize).toArray();

            // Update (one update op even if multiple doucuments match)
            assert.commandWorked(collection.updateMany({a: {$lt: insertCount}}, {$set: {b: 42}}));

            // Delete (one delete op even if multiple documents match)
            // Delete everything so we can compare results of consecutive exerciseOperations.
            assert.commandWorked(collection.deleteMany({a: {$lt: insertCount}}));
        }

        const coll = dbTest.getCollection("opcounters");
        coll.drop();

        const initialOpcounters = dbTest.serverStatus().opcounters;
        jsTestLog("opcounters before running tests: " + tojson(initialOpcounters));

        // Execute operations in commands mode against "coll" collection and cache the counters.
        exerciseOperations(coll);
        const afterCommands = dbTest.serverStatus().opcounters;
        jsTestLog("opcounters after running in commands mode: " + tojson(afterCommands));
        assert.eq(undefined, afterCommands.deprecated, "should have no deprecated ops yet");

        const queryCommands = afterCommands.query - initialOpcounters.query;
        const insertCommands = afterCommands.insert - initialOpcounters.insert;
        const getmoreCommands = afterCommands.getmore - initialOpcounters.getmore;
        const deleteCommands = afterCommands.delete - initialOpcounters.delete;
        const updateCommands = afterCommands.update - initialOpcounters.update;

        // Switch to legacy mode and execute exactly the same operations. This should add deprecated
        // section to serverStatus with the same counts as we got in `afterCommands` and it should
        // double the base counts.
        const mongo = dbTest.getMongo();
        mongo.forceReadMode("legacy");
        mongo.forceWriteMode("legacy");
        exerciseOperations(coll);

        const afterLegacyOps = dbTest.serverStatus().opcounters;
        jsTestLog("opcounters after running in legacy mode: " + tojson(afterLegacyOps));

        const queryDeprecated = afterLegacyOps.deprecated.query;
        const insertDeprecated = afterLegacyOps.deprecated.insert;
        const getmoreDeprecated = afterLegacyOps.deprecated.getmore;
        const deleteDeprecated = afterLegacyOps.deprecated.delete;
        const updateDeprecated = afterLegacyOps.deprecated.update;
        const totalDeprecated = insertDeprecated + queryDeprecated + getmoreDeprecated +
            updateDeprecated + deleteDeprecated;

        // Check that the legacy operations have been accounted for in the deprecated counters and
        // that they've produced the same numbers as command-based ops.
        assert.eq(insertCommands, insertDeprecated, "deprecated insert");
        assert.eq(queryCommands, queryDeprecated, "deprecated query");
        assert.eq(getmoreCommands, getmoreDeprecated, "deprecated getmore");
        assert.eq(updateCommands, updateDeprecated, "deprecated update");
        assert.eq(deleteCommands, deleteDeprecated, "deprecated delete");
        assert.eq(0, afterLegacyOps.deprecated.killcursors, "deprecated killcursors");
        assert.eq(totalDeprecated, afterLegacyOps.deprecated.total, "deprecated total");

        // Check that the legacy operations have been added to the main counters.
        assert.eq(afterCommands.insert + insertDeprecated, afterLegacyOps.insert, "main insert");
        assert.eq(afterCommands.query + queryDeprecated, afterLegacyOps.query, "main query");
        assert.eq(
            afterCommands.getmore + getmoreDeprecated, afterLegacyOps.getmore, "main getmore");
        assert.eq(afterCommands.update + updateDeprecated, afterLegacyOps.update, "main update");
        assert.eq(afterCommands.delete + deleteDeprecated, afterLegacyOps.delete, "main delete");

        // Check killcursors separately to make the math above simpler.
        assert.commandWorked(coll.insertMany([{a: 0}, {a: 1}, {a: 2}, {a: 3}, {a: 4}]));
        let cursor = getLegacyCursor(dbTest, coll);
        assert.eq(cursor.hasNext(), true);
        cursor.close();
        assert.eq(1, dbTest.serverStatus().opcounters.deprecated.killcursors, "killcursors");

        tearDown(conn);
    };
};
