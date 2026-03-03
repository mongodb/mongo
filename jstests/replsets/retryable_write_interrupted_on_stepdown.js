/**
 * Tests that retried writes are correctly interrupted on stepdown on the first and second try (see: SERVER-110728).
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("Tests that retried writes are correctly interrupted on stepdown on the first and second try", function () {
    const kNodes = 3;
    const dbName = "test_db";
    const collName = jsTest.name();

    let rst = null;
    let primaryConn = null;
    let primaryDB = null;

    beforeEach(() => {
        clearRawMongoProgramOutput();

        rst = new ReplSetTest({
            nodes: kNodes,
            settings: {chainingAllowed: false},
        });
        rst.startSet({});
        rst.initiate();

        primaryConn = rst.getPrimary();
        primaryDB = primaryConn.getDB(dbName);

        primaryDB.adminCommand({
            "setDefaultRWConcern": 1,
            "defaultWriteConcern": {
                "w": "majority",
                // High timeout to give time for the other thread to execute the stepdown command.
                "wtimeout": 10000,
            },
            "defaultReadConcern": {
                "level": "local",
            },
        });

        assert.commandWorked(primaryDB.createCollection("coordinationColl", {writeConcern: {w: 1}}));
        primaryDB.getMongo().setSecondaryOk();
        primaryDB.getMongo().setReadPref("primaryPreferred");

        assert.commandWorked(primaryDB.createCollection(collName));
        assert.commandWorked(
            primaryDB[collName].insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
            ]),
        );
        assert.eq(primaryDB[collName].countDocuments({}), 2);
        rst.awaitReplication();
        rst.getSecondaries().forEach((secondary) => assert.eq(secondary.getDB(dbName)[collName].countDocuments({}), 2));
    });

    afterEach(() => {
        rst.stopSet();
    });

    // Functions used to signal that an event has happened.
    // Used to coordinate writes and stepdown attempts between threads.
    function signalEvent(primaryDB, eventName) {
        assert.commandWorked(primaryDB.coordinationColl.insertOne({_id: eventName}, {writeConcern: {w: 1}}));
    }
    function waitForEvent(primaryDB, eventName) {
        assert.soon(
            () => primaryDB.coordinationColl.findOne({_id: eventName}),
            "Did not find '" + eventName + "' document in 'coordinationColl', the event was never signaled",
        );
    }

    // Functions used to run the test with different write commands:

    function doTestForInsert(testRetryWriteFn) {
        it("retryable INSERT write gets interrupted", () => {
            jsTest.log.info("Test retryable write for INSERT");

            function runInsert(primaryDB, collName) {
                const insertCmd = {
                    insert: collName,
                    documents: [{_id: 10}, {_id: 20}],
                    ordered: false,
                    lsid: {id: UUID("867dee52-c331-484e-92d1-c56479b8e67e")},
                    txnNumber: NumberLong(42),
                };
                return primaryDB.runCommand(insertCmd);
            }

            assert.eq(primaryDB[collName].countDocuments({}), 2);
            testRetryWriteFn(runInsert);
            rst.nodes.forEach((n) => assert.eq(n.getDB(dbName)[collName].countDocuments({}), 4));
        });
    }

    function doTestForUpdate(testRetryWriteFn) {
        it("retryable UPDATE write gets interrupted", () => {
            jsTest.log.info("Test retryable write for UPDATE");

            function runUpdate(primaryDB, collName) {
                const updateCmd = {
                    update: collName,
                    updates: [
                        {q: {_id: 1}, u: {$inc: {x: 1}}}, // in place
                        {q: {_id: 2}, u: {z: 1}}, // replacement
                        {q: {_id: 3}, u: {$inc: {y: 1}}, upsert: true},
                    ],
                    ordered: false,
                    lsid: {id: UUID("867dee52-c331-484e-92d1-c56479b8e67e")},
                    txnNumber: NumberLong(42),
                };
                return primaryDB.runCommand(updateCmd);
            }

            assert.eq(primaryDB[collName].countDocuments({}), 2);
            testRetryWriteFn(runUpdate);
            rst.nodes.forEach((n) => assert.eq(n.getDB(dbName)[collName].countDocuments({}), 3));
        });
    }

    function doTestForDelete(testRetryWriteFn) {
        it("retryable DELETE write gets interrupted", () => {
            jsTest.log.info("Test retryable write for DELETE");

            function runDelete(primaryDB, collName) {
                const deleteCmd = {
                    delete: collName,
                    deletes: [
                        {q: {x: 1}, limit: 1},
                        {q: {_id: 2}, limit: 1},
                    ],
                    ordered: false,
                    lsid: {id: UUID("867dee52-c331-484e-92d1-c56479b8e67e")},
                    txnNumber: NumberLong(42),
                };
                return primaryDB.runCommand(deleteCmd);
            }

            assert.eq(primaryDB[collName].countDocuments({}), 2);
            testRetryWriteFn(runDelete);
            rst.nodes.forEach((n) => assert.eq(n.getDB(dbName)[collName].countDocuments({}), 0));
        });
    }

    function doTestForBulkWrite(testRetryWriteFn) {
        it("retryable BULK write gets interrupted", () => {
            jsTest.log.info("Test retryable write for BULK write");

            function runBulkWrite(primaryDB, collName) {
                const bulkCmd = {
                    bulkWrite: 1,
                    ops: [
                        {insert: 0, document: {_id: 10}},
                        {insert: 0, document: {_id: 20}},
                        {update: 0, filter: {_id: 1}, updateMods: {$inc: {x: 1}}},
                        {delete: 0, filter: {_id: 2}},
                    ],
                    nsInfo: [{ns: primaryDB[collName].getFullName()}],
                    lsid: {id: UUID("867dee52-c331-484e-92d1-c56479b8e67e")},
                    txnNumber: NumberLong(42),
                };
                return primaryDB.adminCommand(bulkCmd);
            }

            assert.eq(primaryDB[collName].countDocuments({}), 2);
            testRetryWriteFn(runBulkWrite);
            rst.nodes.forEach((n) => assert.eq(n.getDB(dbName)[collName].countDocuments({}), 3));
        });
    }

    //
    // Retryable write on the same node.
    //
    describe("Execute retried writes twice on the same node", function () {
        /**
         * Run a retryable write on the primary while doing a stepdown to interrupt the write.
         *
         * We first block both secondaries from applying oplogs, this will prevent majority writes
         * from completing.
         * We have two threads:
         * 1. The "doRetryableWrite" thread will do the retryable write twice. The write will wait
         *    for write majority for 10sec. Before the write times out the other thread will attempt
         *    to step-down which will make the writes immediately fail with the
         *    InterruptedDueToReplStateChange error.
         * 2. The main thread will wait for the "doRetryableWrite" thread to execute the write
         *    command, then it will attempt to step-down. The step-down will make the write fail and
         *    then it will timeout after 1sec (secondaryCatchUpPeriodSecs) since no secondaries can
         *    catch up with the primary.
         *
         * After the "doRetryableWrite" thread has attempted to execute the write twice, we join the
         * thread. Then we unlock both secondaries and execute the write a third time, this attempt
         * will succeed.
         *
         * @param {function (primaryDB, collName) -> WriteResult} runWrite - Run a retryable write
         */
        function testRetryWriteOnPrimary(runWrite) {
            // Block secondaries from applying oplogs
            rst.getSecondaries().forEach((secondary) => secondary.getDB(dbName).fsyncLock());

            function doRetryableWrite(primaryPort, dbName, collName, runWrite, signalEvent, waitForEvent) {
                const newConn = new Mongo("localhost:" + primaryPort);
                assert(newConn);
                newConn.setSecondaryOk();
                newConn.setReadPref("primaryPreferred");
                const primaryDB = newConn.getDB(dbName);

                signalEvent(primaryDB, "readyForFirstStepDown");
                // First try:
                let writeRes = assert.commandFailedWithCode(
                    runWrite(primaryDB, collName),
                    ErrorCodes.InterruptedDueToReplStateChange,
                );
                jsTest.log.info("1st write result: " + tojson(writeRes));

                waitForEvent(primaryDB, "firstStepDownDone");

                // Second try:
                // Before SERVER-110728, the second try would fail with "WriteConcernTimeout" error
                // instead of "InterruptedDueToReplStateChange".
                writeRes = assert.commandFailedWithCode(
                    runWrite(primaryDB, collName),
                    ErrorCodes.InterruptedDueToReplStateChange,
                );
                jsTest.log.info("2nd write result: " + tojson(writeRes));
            }

            const writerThread = new Thread(
                doRetryableWrite,
                primaryConn.port,
                dbName,
                collName,
                runWrite,
                signalEvent,
                waitForEvent,
            );
            writerThread.start();

            waitForEvent(primaryDB, "readyForFirstStepDown");
            sleep(1000); // Give time to the other thread to execute the write attempt

            let result = assert.commandFailedWithCode(
                primaryConn.adminCommand({replSetStepDown: 5, secondaryCatchUpPeriodSecs: 1, force: false}),
                ErrorCodes.ExceededTimeLimit,
            );
            jsTest.log.info("1st stepdown, result: " + tojson(result));
            assert.soon(
                () =>
                    rawMongoProgramOutput(
                        "8562701.*Repl state change interrupted a thread.*" +
                            '"name":"conn[0-9]+".*globalLockConflict":true.*"isRetryableWrite":true',
                    ),
                "mongod did not log that it interrupted the retryable write due to the stepdown",
            );

            signalEvent(primaryDB, "firstStepDownDone");
            sleep(1000); // Give time to the other thread to execute the write attempt

            result = assert.commandFailedWithCode(
                primaryConn.adminCommand({replSetStepDown: 5, secondaryCatchUpPeriodSecs: 1, force: false}),
                ErrorCodes.ExceededTimeLimit,
            );
            jsTest.log.info("2nd stepdown, result:" + tojson(result));
            assert.soon(
                () =>
                    rawMongoProgramOutput(
                        "8562701.*Repl state change interrupted a thread.*" +
                            '"name":"conn[0-9]+".*"globalLockConflict":false.*"isRetryableWrite":true',
                    ),
                "mongod did not log that it interrupted the retryable write due to the stepdown",
            );

            assert.doesNotThrow(() => writerThread.join());

            // Unlock secondaries
            rst.getSecondaries().forEach((secondary) => secondary.getDB(dbName).fsyncUnlock());

            // Now the write should succeed
            assert.commandWorked(runWrite(primaryDB, collName));

            rst.awaitReplication();
        }

        doTestForInsert(testRetryWriteOnPrimary);
        doTestForUpdate(testRetryWriteOnPrimary);
        doTestForDelete(testRetryWriteOnPrimary);
        doTestForBulkWrite(testRetryWriteOnPrimary);
    }); // "Execute retried writes twice on the same node"

    //
    // Retryable write on different nodes.
    //
    describe("Execute retried writes on different nodes", function () {
        let lockedSecondary = null;
        let liveSecondary = null;

        beforeEach(() => {
            const secondaries = rst.getSecondaries();
            assert.eq(secondaries.length, 2);
            lockedSecondary = secondaries[0];
            liveSecondary = secondaries[1];

            primaryDB.adminCommand({
                "setDefaultRWConcern": 1,
                "defaultWriteConcern": {
                    // Writes should be blocked even with one live secondary
                    "w": 3,
                    // High timeout to give time for the other thread to execute the stepdown command.
                    "wtimeout": 10000,
                },
            });
        });

        /**
         * Run a retryable write twice on a different primary while doing a stepdown to interrupt
         * the write.
         *
         * We first block one of the secondaries from applying oplogs, this will prevent w:3 writes
         * from completing, and will allow the step-down to succeed.
         * We have two threads:
         * 1. The "doRetryableWrite" thread will do the retryable write (with write concern w:3)
         *    twice. The write will wait to be replicated on all 3 nodes of the replset for 10sec.
         *    Before the write times out the other thread will do a step-down which will make the
         *    writes immediately fail with the InterruptedDueToReplStateChange error. After the
         *    first write has failed and the step-down is completed, this thread will re-try the
         *    write on the new primary. Again, the other thread will do another step-down before the
         *    write can time out, which will make it fail.
         * 2. The main thread will wait for the "doRetryableWrite" thread to execute the write
         *    command, then it will send a step-down command on the current primary. The step-down
         *    will succeed and make the write fail. The first step-down is executed on "primary" and
         *    the second is executed on "liveSecondary", since this is the secondary node which will
         *    step-up on the first step-down.
         *
         * After the "doRetryableWrite" thread has attempted to execute the write twice, we join the
         * thread. Then we unlock the secondary and execute the write a third time, this attempt
         * will succeed.
         *
         * @param {function (primaryDB, collName) -> WriteResult} runWrite - Run a retryable write
         */
        function testRetryWriteOnTwoNodes(runWrite) {
            // Block one secondary from applying oplogs
            lockedSecondary.getDB(dbName).fsyncLock();

            function doRetryableWrite(
                primaryPort,
                liveSecondaryPort,
                dbName,
                collName,
                runWrite,
                signalEvent,
                waitForEvent,
            ) {
                const primaryConn = new Mongo("localhost:" + primaryPort);
                assert(primaryConn);
                primaryConn.setSecondaryOk();
                primaryConn.setReadPref("primaryPreferred");
                const primaryDB = primaryConn.getDB(dbName);

                signalEvent(primaryDB, "readyForFirstStepDown");
                // First try:
                let writeRes = assert.commandFailedWithCode(
                    runWrite(primaryDB, collName),
                    ErrorCodes.InterruptedDueToReplStateChange,
                );
                jsTest.log.info("1st write result: " + tojson(writeRes));

                const newPrimaryConn = new Mongo("localhost:" + liveSecondaryPort);
                assert(newPrimaryConn);
                newPrimaryConn.setSecondaryOk();
                newPrimaryConn.setReadPref("primaryPreferred");
                assert.soon(
                    () => newPrimaryConn.adminCommand("hello").isWritablePrimary,
                    "Live secondary node never became a primary after the first step-down",
                );
                const newPrimaryDB = newPrimaryConn.getDB(dbName);
                waitForEvent(newPrimaryDB, "readyForSecondStepDown");

                // Second try:
                // Before SERVER-110728, the second try would fail with "PrimarySteppedDown" error
                // instead of "InterruptedDueToReplStateChange".
                writeRes = assert.commandFailedWithCode(
                    runWrite(newPrimaryDB, collName),
                    ErrorCodes.InterruptedDueToReplStateChange,
                );
                jsTest.log.info("2nd write result: " + tojson(writeRes));
            }

            const writerThread = new Thread(
                doRetryableWrite,
                primaryConn.port,
                liveSecondary.port,
                dbName,
                collName,
                runWrite,
                signalEvent,
                waitForEvent,
            );
            writerThread.start();

            waitForEvent(primaryDB, "readyForFirstStepDown");
            sleep(1000); // Give time to the other thread to execute the write attempt

            let result = assert.commandWorked(
                primaryConn.adminCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 5, force: false}),
            );
            jsTest.log.info("1st stepdown, result: " + tojson(result));
            assert.soon(
                () =>
                    rawMongoProgramOutput(
                        "8562701.*Repl state change interrupted a thread.*" +
                            '"name":"conn[0-9]+".*"globalLockConflict":true.*"isRetryableWrite":true',
                    ),
                "mongod did not log that it interrupted the retryable write due to the stepdown",
            );

            assert.soon(
                () => liveSecondary.adminCommand("hello").isWritablePrimary,
                "Live secondary node never became a primary after the first step-down",
            );
            assert.soon(
                () => !primaryConn.adminCommand("hello").isWritablePrimary,
                "The old primary node never stepped-down",
            );
            const newPrimary = liveSecondary;

            // Wait for at least replSetStepDown time (10sec) so that the old primary can become primary again.
            sleep(10000);
            signalEvent(newPrimary.getDB(dbName), "readyForSecondStepDown");
            sleep(1000); // Give time to the other thread to execute the write attempt

            result = assert.commandWorked(
                newPrimary.adminCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 5, force: false}),
            );
            jsTest.log.info("2nd stepdown, result:" + tojson(result));
            assert.soon(
                () =>
                    rawMongoProgramOutput(
                        "8562701.*Repl state change interrupted a thread.*" +
                            '"name":"conn[0-9]+".*"globalLockConflict":false.*"isRetryableWrite":true',
                    ),
                "mongod did not log that it interrupted the retryable write due to the stepdown",
            );

            assert.doesNotThrow(() => writerThread.join());

            assert.soon(
                () => primaryConn.adminCommand("hello").isWritablePrimary,
                "Old primary node never became a primary after the second step-down",
            );
            assert.soon(
                () => !liveSecondary.adminCommand("hello").isWritablePrimary,
                "Live secondary node never stepped down after the second step-down",
            );

            // Unlock secondary
            lockedSecondary.getDB(dbName).fsyncUnlock();

            // Now the write should succeed
            assert.commandWorked(runWrite(primaryDB, collName));

            rst.awaitReplication();
        }

        doTestForInsert(testRetryWriteOnTwoNodes);
        doTestForUpdate(testRetryWriteOnTwoNodes);
        doTestForDelete(testRetryWriteOnTwoNodes);
        doTestForBulkWrite(testRetryWriteOnTwoNodes);
    }); // "Execute retried writes on different nodes"
});
