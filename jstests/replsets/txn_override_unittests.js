/**
 * Verifies the network_error_and_txn_override passthrough behaves correctly.
 * network_error_and_txn_override groups sequential CRUD commands into transactions and commits them
 * when a DDL command arrives. When used with auto_retry_on_network_error, it retries transactions
 * on network errors. This tests that errors are surfaced correctly and retried correctly in various
 * error cases.
 *
 * Commands are set to fail in two ways: 1. 'failCommand' failpoint Commands that fail with this
 * failpoint generally fail before running the command, with the exception of WriteConcernErrors
 * which occur after the command has run. This also allows us to close the connection on the server
 * and simulate network errors.
 *
 *     2. 'runCommand' override This completely mocks out responses from the server without running
 *     a command at all. This is mainly used for generating a WriteConcernError without running the
 *     command at all, simulating a WriteConcernError on a command that later gets rolled back.
 *
 * The test also makes use of "post-command functions" for injecting functionality at times when the
 * test is not given control. Since network_error_and_txn_override.js may run multiple commands when
 * instructed to run a single command (for retries for example), it is important to be able to run
 * functions in between those extra commands. To do so, we add a hook through the 'runCommand'
 * override that runs a given function after a specific command.
 *
 * Many of these tests assert that a command fails but do not care with which code. This is
 * intentional since it doesn't really matter how the test fails as long as it fails and this allows
 * us to keep more tests running now. That said, these should ideally all throw so we do not rely on
 * the test itself calling assert.commandWorked.
 *
 * @tags: [requires_replication, uses_transactions]
 */
(function() {
"use strict";
load("jstests/libs/transactions_util.js");
load('jstests/libs/write_concern_util.js');

// Commands not to override since they can log excessively.
const runCommandOverrideBlacklistedCommands =
    ["getCmdLineOpts", "serverStatus", "configureFailPoint"];

// cmdResponseOverrides is a map from commands to responses that should be provided in lieu of
// running the command on the server. This is mostly used for returning WriteConcernErrors
// without running the command or returning WriteConcernErrors with top level errors.
// {<cmdName>: {responseObj: <response object>}}
let cmdResponseOverrides = {};

// postCommandFuncs is a map from commands to functions that should be run after either mocking
// out their response or running them on the server. This is used to inject functionality at
// times when the test is not given control, such as when the override runs extra commands on
// retries.
// {<cmdName>: {func}}
let postCommandFuncs = {};

/**
 * Deletes the command override from the given command.
 */
function clearCommandOverride(cmdName) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    delete cmdResponseOverrides[cmdName];
}

/**
 * Deletes the post-command function for the given command.
 */
function clearPostCommandFunc(cmdName) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    delete postCommandFuncs[cmdName];
}

/**
 * Clears all command overrides and post-command functions.
 */
function clearAllCommandOverrides() {
    cmdResponseOverrides = {};
    postCommandFuncs = {};
}

/**
 * Sets the provided function as the post-command function for the given command.
 */
function attachPostCmdFunction(cmdName, func) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    postCommandFuncs[cmdName] = func;
}

/**
 * Sets that the given command should return the given response. The command will not actually
 * be run.
 */
function setCommandMockResponse(cmdName, mockResponse) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    cmdResponseOverrides[cmdName] = {responseObj: mockResponse};
}

/**
 * Sets that the given command should fail with ok:1 and the given write concern error.
 * The command will not actually be run.
 */
function failCommandWithWCENoRun(cmdName, writeConcernErrorCode, writeConcernErrorCodeName) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    cmdResponseOverrides[cmdName] = {
        responseObj: {
            ok: 1,
            writeConcernError: {code: writeConcernErrorCode, codeName: writeConcernErrorCodeName}
        }
    };
}

/**
 * Sets that the given command should fail with the given error and the given write concern
 * error. The command will not actually be run.
 */
function failCommandWithErrorAndWCENoRun(
    cmdName, errorCode, errorCodeName, writeConcernErrorCode, writeConcernErrorCodeName) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    cmdResponseOverrides[cmdName] = {
        responseObj: {
            ok: 0,
            code: errorCode,
            codeName: errorCodeName,
            writeConcernError: {code: writeConcernErrorCode, codeName: writeConcernErrorCodeName}
        }
    };
}

/**
 * Run the post-command function for the given command, if one has been set, and clear it once
 * used.
 */
function runPostCommandFunc(cmdName) {
    assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

    if (postCommandFuncs[cmdName]) {
        jsTestLog("Running post-command function for " + cmdName);
        try {
            postCommandFuncs[cmdName]();
        } finally {
            clearPostCommandFunc(cmdName);
        }
    }
}

/**
 * Overrides 'runCommand' to provide a specific pre-set response to the given command. If the
 * command is in the blacklist, it is not overridden. Otherwise, if a command response has been
 * specified, returns that without running the function. If a post-command function is specified
 * for the command, runs that after the command is run. The post-command function is run
 * regardless of whether the command response was overridden or not.
 */
const mongoRunCommandOriginal = Mongo.prototype.runCommand;
Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    const cmdName = Object.keys(cmdObj)[0];
    if (runCommandOverrideBlacklistedCommands.includes(cmdName)) {
        return mongoRunCommandOriginal.apply(this, arguments);
    }

    if (cmdResponseOverrides.hasOwnProperty(cmdName)) {
        const cmdResponse = cmdResponseOverrides[cmdName];
        // Overrides are single-use.
        clearCommandOverride(cmdName);
        assert(cmdResponse);

        jsTestLog("Unittest returning: " + tojsononeline(cmdResponse.responseObj) +
                  ", running: " + tojsononeline(cmdObj));
        assert(cmdResponse.responseObj);
        assert(cmdResponse.responseObj.ok === 1 || cmdResponse.responseObj.ok === 0);

        runPostCommandFunc(cmdName);
        return cmdResponse.responseObj;
    }

    const res = mongoRunCommandOriginal.apply(this, arguments);
    print("Unittest received: " + tojsononeline(res) + ", running: " + tojsononeline(cmdObj));
    runPostCommandFunc(cmdName);
    return res;
};

const dbName = "txn_override_unittests";
const collName1 = "test_coll1";
const collName2 = "test_coll2";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const conn = rst.getPrimary();

// We have a separate connection for the failpoint so that it does not break up the transaction
// buffered in network_error_and_txn_override.js.
const failpointConn = new Mongo(conn.host);

/**
 * Marks that the given command should fail with the given parameters using the failCommand
 * failpoint. This does not break up a currently active transaction in the override function.
 * This does override previous uses of the failpoint, however.
 */
function failCommandWithFailPoint(commandsToFail, {
    errorCode: errorCode,
    closeConnection: closeConnection = false,
    writeConcernError: writeConcernError,
    // By default only fail the next request of the given command.
    mode: mode = {
        times: 1
    },
} = {}) {
    // The fail point will ignore the WCE if an error code is specified.
    assert(!(writeConcernError && errorCode),
           "Cannot specify both a WCE " + tojsononeline(writeConcernError) + " and an error code " +
               errorCode);

    let data = {
        failCommands: commandsToFail,
    };

    if (errorCode) {
        data["errorCode"] = errorCode;
    }

    if (closeConnection) {
        data["closeConnection"] = closeConnection;
    }

    if (writeConcernError) {
        data["writeConcernError"] = writeConcernError;
    }

    assert.commandWorked(mongoRunCommandOriginal.apply(
        failpointConn, ['admin', {configureFailPoint: "failCommand", mode: mode, data: data}, 0]));
}

/**
 * Turns off the failCommand failpoint completely.
 */
function stopFailingCommands() {
    assert.commandWorked(mongoRunCommandOriginal.apply(
        failpointConn, ['admin', {configureFailPoint: "failCommand", mode: "off"}, 0]));
}

/**
 * Run a 'ping' command that is not allowed in a transaction. This has no effect, but causes
 * network_error_and_txn_override.js to commit the current transaction in order to run the
 * 'ping'.
 */
function endCurrentTransactionIfOpen() {
    print("=-=-=-= Ending current transaction if open");
    assert.commandWorked(testDB.runCommand({ping: 1}));
}

/**
 * Aborts the current transaction in network_error_and_txn_override.js.
 */
function abortCurrentTransaction() {
    const session = testDB.getSession();
    const lsid = session.getSessionId();
    const txnNum = TestData.currentTxnOverrideTxnNumber;
    print("=-=-=-= Aborting current transaction " + txnNum + " on " + tojsononeline(lsid));

    assert.commandWorked(mongoRunCommandOriginal.apply(
        testDB.getMongo(),
        ['admin', {abortTransaction: 1, autocommit: false, lsid: lsid, txnNumber: txnNum}, 0]));
}

/**
 * Runs a test where a transaction attempts to use a forbidden database name. When running a
 * CRUD operation on one of these databases, network_error_and_txn_override.js is expected to
 * commit the current transaction and run the CRUD operation outside of a transaction.
 */
function testBadDBName(session, badDBName) {
    const badDB = session.getDatabase(badDBName);
    const badColl = badDB['foo'];
    assert.commandWorked(badDB.createCollection(collName1));

    assert.commandWorked(coll1.insert({_id: 1}));
    assert.eq(coll1.find().itcount(), 1);

    assert.commandWorked(badColl.insert({_id: 1}));
    assert.eq(coll1.find().itcount(), 1);
    assert.eq(badColl.find().itcount(), 1);

    // We attempt another insert in the 'bad collection' that gets a 'DuplicateKey' error.
    // 'DuplicateKey' errors cause transactions to abort, so if this error were received in a
    // transaction, we would expect the transaction to get aborted and the collections to be
    // empty. Since this is not running in a transaction, even though the statement fails, the
    // previous inserts do not storage-rollback.
    assert.commandFailedWithCode(badColl.insert({_id: 1}), ErrorCodes.DuplicateKey);
    assert.eq(coll1.find().itcount(), 1);
    assert.eq(badColl.find().itcount(), 1);
}

/**
 * Runs a specific test case, resetting test state before and after.
 */
function runTest(testSuite, testCase) {
    // Drop with majority write concern to ensure transactions in subsequent test cases can
    // immediately take locks on either collection.
    coll1.drop({writeConcern: {w: "majority"}});
    coll2.drop({writeConcern: {w: "majority"}});

    // Ensure all overrides and failpoints have been turned off before running the test.
    clearAllCommandOverrides();
    stopFailingCommands();

    jsTestLog(testSuite + ": Testing " + testCase.name);
    testCase.test();

    // End the current transaction if the test did not end it itself.
    endCurrentTransactionIfOpen();
    jsTestLog(testSuite + ": Test " + testCase.name + " complete.");

    // Ensure all overrides and failpoints have been turned off after running the test as well.
    clearAllCommandOverrides();
    stopFailingCommands();
}

const retryOnNetworkErrorTests = [
    {
        name: "update with network error after success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            attachPostCmdFunction("update", function() {
                throw new Error("SocketException");
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "ordinary CRUD ops",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(testDB.runCommand({insert: collName1, documents: [{_id: 2}]}));
            assert.eq(coll1.find().itcount(), 2);
        }
    },
    {
        name: "retry on NotMaster",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "retry on NotMaster ordered",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.NotMaster});
            assert.commandFailed(
                testDB.runCommand({insert: collName1, documents: [{_id: 2}], ordered: true}));
        }
    },
    {
        name: "retry on NotMaster with object change",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            let obj1 = {_id: 1, x: 5};
            let obj2 = {_id: 2, x: 5};
            assert.commandWorked(coll1.insert(obj1));
            assert.commandWorked(coll1.insert(obj2));
            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 5}]);
            obj1.x = 7;
            assert.commandWorked(coll1.update({_id: 2}, {$set: {x: 8}}));
            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 8}]);
        }
    },
    {
        name: "implicit collection creation with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(
                ["insert"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal stepdown error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun(
                "insert", ErrorCodes.NotMaster, "NotMaster", ErrorCodes.NotMaster, "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal ordinary error",
        test: function() {
            failCommandWithErrorAndWCENoRun("insert",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with ordinary error",
        test: function() {
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.OperationFailed});
            assert.commandFailed(coll1.insert({_id: 1}));
        }
    },
    {
        name: "implicit collection creation with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["insert"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError no success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithWCENoRun("insert", ErrorCodes.NotMaster, "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "update with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with ordinary error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.OperationFailed});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandFailed(coll1.update({_id: 1}, {$inc: {x: 1}}));
        }
    },
    {
        name: "update with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with two stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"],
                                     {errorCode: ErrorCodes.NotMaster, mode: {times: 2}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "update with chained stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            // Chain multiple update errors together.
            attachPostCmdFunction("update", function() {
                failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "commands not run in transactions",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.commandFailedWithCode(coll1.insert({_id: 1}), ErrorCodes.DuplicateKey);

            // If this were run in a transaction, the original insert and the duplicate one would
            // both be storage-rolled-back and the count would be 0. We test that the count is 1
            // to prove that the inserts are not in a transaction.
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "transaction commands not retried on retryable code",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});

            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            session.startTransaction();
            assert.commandFailedWithCode(
                testDB.runCommand({update: collName1, updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}]}),
                ErrorCodes.NotMaster);
            assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);

            assert.eq(coll1.find().toArray(), [{_id: 1}]);
        }
    },
    {
        name: "transaction commands not retried on network error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {closeConnection: true});

            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            session.startTransaction();
            const error = assert.throws(() => {
                return testDB.runCommand(
                    {update: collName1, updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}]});
            });
            assert(isNetworkError(error), tojson(error));
            assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);

            assert.eq(coll1.find().toArray(), [{_id: 1}]);
        }
    },
    {
        name: "commitTransaction retried on retryable code",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {errorCode: ErrorCodes.NotMaster});

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            assert.commandWorked(session.commitTransaction_forTesting());

            assert.eq(coll1.find().toArray(), [{_id: 1}]);
        }
    },
    {
        name: "commitTransaction retried on write concern error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {
                writeConcernError:
                    {code: ErrorCodes.PrimarySteppedDown, codeName: "PrimarySteppedDown"}
            });

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            const res = assert.commandWorked(session.commitTransaction_forTesting());
            assert(!res.hasOwnProperty("writeConcernError"));

            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "commitTransaction not retried on transient transaction error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            // Abort the transaction so the commit receives NoSuchTransaction. Note that the fail
            // command failpoint isn't used because it returns without implicitly aborting the
            // transaction.
            const lsid = session.getSessionId();
            const txnNumber = NumberLong(session.getTxnNumber_forTesting());
            assert.commandWorked(testDB.adminCommand(
                {abortTransaction: 1, lsid, txnNumber, autocommit: false, stmtId: NumberInt(0)}));

            const res = assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                                     ErrorCodes.NoSuchTransaction);
            assert.eq(["TransientTransactionError"], res.errorLabels);

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "commitTransaction retried on network error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {closeConnection: true});

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            assert.commandWorked(session.commitTransaction_forTesting());

            assert.eq(coll1.find().toArray(), [{_id: 1}]);
        }
    },
    {
        name: "abortTransaction retried on retryable code",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["abortTransaction"], {errorCode: ErrorCodes.NotMaster});

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            assert.commandWorked(session.abortTransaction_forTesting());

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "abortTransaction retried on network error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["abortTransaction"], {closeConnection: true});

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            assert.commandWorked(session.abortTransaction_forTesting());

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "abortTransaction retried on write concern error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["abortTransaction"], {
                writeConcernError:
                    {code: ErrorCodes.PrimarySteppedDown, codeName: "PrimarySteppedDown"}
            });

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            // The fail command fail point with a write concern error triggers after the command
            // is processed, so the retry will find the transaction has already aborted and return
            // NoSuchTransaction.
            const res = assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                                     ErrorCodes.NoSuchTransaction);
            assert(!res.hasOwnProperty("writeConcernError"));

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "abortTransaction not retried on transient transaction error",
        test: function() {
            const session = testDB.getSession();

            assert.commandWorked(testDB.createCollection(collName1));

            session.startTransaction();
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);

            // Abort the transaction so the commit receives NoSuchTransaction. Note that the fail
            // command failpoint isn't used because it returns without implicitly aborting the
            // transaction.
            const lsid = session.getSessionId();
            const txnNumber = NumberLong(session.getTxnNumber_forTesting());
            assert.commandWorked(testDB.adminCommand(
                {abortTransaction: 1, lsid, txnNumber, autocommit: false, stmtId: NumberInt(0)}));

            const res = assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                                     ErrorCodes.NoSuchTransaction);
            assert.eq(["TransientTransactionError"], res.errorLabels);

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "raw response w/ one retryable error",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                raw: {
                    shardOne: {code: ErrorCodes.NotMaster, errmsg: "dummy"},
                    shardTwo: {code: ErrorCodes.InternalError, errmsg: "dummy"}
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));

            // The first attempt should fail, but the retry succeeds.
            assert.commandWorked(coll1.createIndex({x: 1}));

            // The index should exist.
            const indexes = coll1.getIndexes();
            assert.eq(2, indexes.length, tojson(indexes));
            assert(indexes.some(idx => idx.name === "x_1"), tojson(indexes));
        }
    },
    {
        name: "raw response w/ one retryable error and one success",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                raw: {
                    // Raw responses only omit a top-level code if more than one error was
                    // returned from a shard, so a third shard is needed.
                    shardOne: {code: ErrorCodes.NotMaster, errmsg: "dummy"},
                    shardTwo: {ok: 1},
                    shardThree: {code: ErrorCodes.InternalError, errmsg: "dummy"},
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));

            // The first attempt should fail, but the retry succeeds.
            assert.commandWorked(coll1.createIndex({x: 1}));

            // The index should exist.
            const indexes = coll1.getIndexes();
            assert.eq(2, indexes.length, tojson(indexes));
            assert(indexes.some(idx => idx.name === "x_1"), tojson(indexes));
        }
    },
    {
        name: "raw response w/ one network error",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                raw: {
                    shardOne: {code: ErrorCodes.InternalError, errmsg: "dummy"},
                    shardTwo: {code: ErrorCodes.HostUnreachable, errmsg: "dummy"}
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));

            // The first attempt should fail, but the retry succeeds.
            assert.commandWorked(coll1.createIndex({x: 1}));

            // The index should exist.
            const indexes = coll1.getIndexes();
            assert.eq(2, indexes.length, tojson(indexes));
            assert(indexes.some(idx => idx.name === "x_1"), tojson(indexes));
        }
    },
    {
        name: "raw response ok:1 w/ retryable write concern error",
        test: function() {
            // The first encountered write concern error from a shard is attached as the top-level
            // write concern error.
            setCommandMockResponse("createIndexes", {
                ok: 1,
                raw: {
                    shardOne: {
                        ok: 1,
                        writeConcernError: {
                            code: ErrorCodes.PrimarySteppedDown,
                            codeName: "PrimarySteppedDown",
                            errmsg: "dummy"
                        }
                    },
                    shardTwo: {ok: 1}
                },
                writeConcernError: {
                    code: ErrorCodes.PrimarySteppedDown,
                    codeName: "PrimarySteppedDown",
                    errmsg: "dummy"
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));

            // The first attempt should fail, but the retry succeeds.
            assert.commandWorked(coll1.createIndex({x: 1}));

            // The index should exist.
            const indexes = coll1.getIndexes();
            assert.eq(2, indexes.length, tojson(indexes));
            assert(indexes.some(idx => idx.name === "x_1"), tojson(indexes));
        }
    },
    {
        name: "raw response w/ no retryable error",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                raw: {
                    shardOne: {code: ErrorCodes.InvalidOptions, errmsg: "dummy"},
                    shardTwo: {code: ErrorCodes.InternalError, errmsg: "dummy"}
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandFailed(coll1.createIndex({x: 1}));
        }
    },
    {
        name: "raw response w/ only acceptable errors",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                code: ErrorCodes.IndexAlreadyExists,
                raw: {
                    shardOne: {code: ErrorCodes.IndexAlreadyExists, errmsg: "dummy"},
                    shardTwo: {ok: 1},
                    shardThree: {code: ErrorCodes.IndexAlreadyExists, errmsg: "dummy"}
                }
            });

            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.createIndex({x: 1}));
        }
    },
    {
        name: "raw response w/ acceptable error and non-acceptable, non-retryable error",
        test: function() {
            setCommandMockResponse("createIndexes", {
                ok: 0,
                raw: {
                    shardOne: {code: ErrorCodes.IndexAlreadyExists, errmsg: "dummy"},
                    shardTwo: {code: ErrorCodes.InternalError, errmsg: "dummy"}
                }
            });

            // "Acceptable" errors are not overridden inside raw reponses.
            assert.commandWorked(testDB.createCollection(collName1));
            const res = assert.commandFailed(coll1.createIndex({x: 1}));
            assert(!res.raw.shardOne.ok, tojson(res));
        }
    },
    {
        name: "shardCollection retryable code buried in error message",
        test: function() {
            setCommandMockResponse("shardCollection", {
                ok: 0,
                code: ErrorCodes.OperationFailed,
                errmsg: "Sharding collection failed :: caused by InterruptedDueToStepdown",
            });

            // Mock a successful response for the retry, since sharding isn't enabled on the
            // underlying replica set.
            attachPostCmdFunction("shardCollection", function() {
                setCommandMockResponse("shardCollection", {
                    ok: 1,
                });
            });

            assert.commandWorked(
                testDB.runCommand({shardCollection: "dummy_namespace", key: {_id: 1}}));
        }
    },
    {
        name: "drop retryable code buried in error message",
        test: function() {
            setCommandMockResponse("drop", {
                ok: 0,
                code: ErrorCodes.OperationFailed,
                errmsg: "Dropping collection failed :: caused by ShutdownInProgress",
            });

            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(testDB.runCommand({drop: collName1}));
        }
    },
];

// These tests only retry on TransientTransactionErrors. All other errors are expected to cause
// the test to fail. Failpoints, overrides, and post-command functions are set by default to
// only run once, so commands should succeed on retry.
const txnOverrideTests = [
    {
        name: "ordinary CRUD ops",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(testDB.runCommand({insert: collName1, documents: [{_id: 2}]}));
            assert.eq(coll1.find().itcount(), 2);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 2);
        }
    },
    {
        name: "getMore in transaction",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll1.insert({_id: 2}));
            assert.eq(coll1.find().itcount(), 2);

            let cmdRes = assert.commandWorked(testDB.runCommand({find: collName1, batchSize: 1}));
            const cursorId = cmdRes.cursor.id;
            assert.gt(cursorId, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.firstBatch.length, 1);

            cmdRes =
                assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: collName1}));
            assert.eq(cmdRes.cursor.id, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.nextBatch.length, 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 2);
        }
    },
    {
        name: "getMore starts transaction",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll1.insert({_id: 2}));
            assert.eq(coll1.find().itcount(), 2);
            assert.eq(coll2.find().itcount(), 0);

            let cmdRes = assert.commandWorked(testDB.runCommand({find: collName1, batchSize: 1}));
            const cursorId = cmdRes.cursor.id;
            assert.gt(cursorId, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.firstBatch.length, 1);

            assert.commandWorked(testDB.createCollection(collName2));

            assert.throws(() => testDB.runCommand({getMore: cursorId, collection: collName1}));
        }
    },
    {
        name: "getMore in different transaction",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll1.insert({_id: 2}));
            assert.eq(coll1.find().itcount(), 2);
            assert.eq(coll2.find().itcount(), 0);

            let cmdRes = assert.commandWorked(testDB.runCommand({find: collName1, batchSize: 1}));
            const cursorId = cmdRes.cursor.id;
            assert.gt(cursorId, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.firstBatch.length, 1);

            // The transactions override commits the current transaction whenever it sees a DDL
            // command.
            assert.commandWorked(testDB.createCollection(collName2));

            assert.commandWorked(coll2.insert({_id: 3}));
            assert.eq(coll1.find().itcount(), 2);
            assert.eq(coll2.find().itcount(), 1);

            assert.commandWorked(coll2.insert({_id: 4}));

            assert.commandFailed(testDB.runCommand({getMore: cursorId, collection: collName1}));
        }
    },
    {
        name: "getMore after TransientTransactionError",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll1.insert({_id: 2}));
            assert.eq(coll1.find().itcount(), 2);
            failCommandWithFailPoint(["find"], {errorCode: ErrorCodes.NoSuchTransaction});

            let cmdRes = assert.commandWorked(testDB.runCommand({find: collName1, batchSize: 1}));
            const cursorId = cmdRes.cursor.id;
            assert.gt(cursorId, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.firstBatch.length, 1);

            cmdRes =
                assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: collName1}));
            assert.eq(cmdRes.cursor.id, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, coll1.getFullName());
            assert.eq(cmdRes.cursor.nextBatch.length, 1);
            assert.eq(coll1.find().itcount(), 2);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 2);
        }
    },
    {
        name: "implicit collection creation",
        test: function() {
            const res = assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(1, res.nInserted);
            assert.eq(coll1.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "errors cause transaction to abort",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.commandFailedWithCode(coll1.insert({_id: 1}), ErrorCodes.DuplicateKey);

            assert.eq(coll1.find().itcount(), 0);
        }
    },
    {
        name: "update with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with ordinary error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.OperationFailed});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandFailed(coll1.update({_id: 1}, {$inc: {x: 1}}));
        }
    },
    {
        name: "update with NoSuchTransaction error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NoSuchTransaction});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.throws(() => coll1.update({_id: 1}, {$inc: {x: 1}}));
        }
    },
    {
        name: "update with two stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"],
                                     {errorCode: ErrorCodes.NotMaster, mode: {times: 2}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "update with chained stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            // Chain multiple update errors together.
            attachPostCmdFunction("update", function() {
                failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "implicit collection creation with stepdown",
        test: function() {
            // We set a failpoint on "create" since an implicit collection creation via
            // findAndModify inside of a transaction will fail and this suite will attempt to
            // explicitly create the collection outside of a transaction, and then retry the
            // entire transaction.
            failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.NotMaster});
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with WriteConcernError",
        test: function() {
            failCommandWithFailPoint(
                ["create"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal stepdown error",
        test: function() {
            failCommandWithErrorAndWCENoRun(
                "create", ErrorCodes.NotMaster, "NotMaster", ErrorCodes.NotMaster, "NotMaster");
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal ordinary error",
        test: function() {
            failCommandWithErrorAndWCENoRun("create",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with ordinary error",
        test: function() {
            failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.OperationFailed});
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with network error",
        test: function() {
            failCommandWithFailPoint(["create"], {closeConnection: true});
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with WriteConcernError no success",
        test: function() {
            failCommandWithWCENoRun("create", ErrorCodes.NotMaster, "NotMaster");
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "errors cause the override to abort transactions",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);

            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.BadValue});
            assert.commandFailedWithCode(coll1.insert({_id: 2}), ErrorCodes.BadValue);

            stopFailingCommands();
            assert.eq(coll1.find().itcount(), 0);

            assert.commandWorked(coll1.insert({_id: 3}));
            assert.eq(coll1.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(
                ["commitTransaction"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal stepdown error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.NotMaster,
                                            "NotMaster",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal ordinary error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with ordinary error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.OperationFailed});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal NoSuchTransaction error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.NoSuchTransaction,
                                            "NoSuchTransaction",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with NoSuchTransaction error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.NoSuchTransaction});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError no success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithWCENoRun("commitTransaction", ErrorCodes.NotMaster, "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commands in 'admin' database end transaction",
        test: function() {
            testBadDBName(session, 'admin');
        }
    },
    {
        name: "commands in 'config' database end transaction",
        test: function() {
            testBadDBName(session, 'config');
        }
    },
    {
        name: "commands in 'local' database end transaction",
        test: function() {
            testBadDBName(session, 'local');
        }
    },
    {
        name: "getMore on change stream executes outside transaction",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));

            // Starting a $changeStream aggregation within a transaction would fail, so the
            // override has to execute this as a standalone command.
            const changeStream = testDB.collName1.watch();
            assert.commandWorked(testDB.collName1.insert({_id: 1}));
            endCurrentTransactionIfOpen();

            // Calling the `next` function on the change stream cursor will trigger a getmore,
            // which the override must also run as a standalone command.
            assert.eq(changeStream.next()["fullDocument"], {_id: 1});

            // An aggregation without $changeStream runs within a transaction.
            let aggCursor = testDB.collName1.aggregate([], {cursor: {batchSize: 0}});
            assert.eq(aggCursor.next(), {_id: 1});

            // Creating a non-$changeStream aggregation cursor and running its getMore in a
            // different transaction will fail.
            aggCursor = testDB.collName1.aggregate([], {cursor: {batchSize: 0}});
            endCurrentTransactionIfOpen();
            assert.throws(() => aggCursor.next());
        }
    },
];

// Failpoints, overrides, and post-command functions are set by default to only run once, so
// commands should succeed on retry.
const txnOverridePlusRetryOnNetworkErrorTests = [
    {
        name: "$where in jstests/core/js4.js",
        test: function() {
            const real = {a: 1, b: "abc", c: /abc/i, d: new Date(111911100111), e: null, f: true};
            assert.commandWorked(coll1.insert(real));

            failCommandWithErrorAndWCENoRun("drop",
                                            ErrorCodes.NamespaceNotFound,
                                            "NamespaceNotFound",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            coll1.drop();
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.NotMaster});

            assert.commandWorked(coll1.insert({a: 2, b: {c: 7, d: "d is good"}}));
            const cursor = coll1.find({
                $where: function() {
                    assert.eq(3, Object.keySet(obj).length);
                    assert.eq(2, obj.a);
                    assert.eq(7, obj.b.c);
                    assert.eq("d is good", obj.b.d);
                    return true;
                }
            });
            assert.eq(1, cursor.toArray().length);
        }
    },
    {
        name: "update with network error after success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            attachPostCmdFunction("update", function() {
                throw new Error("SocketException");
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "retry on NotMaster",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["insert"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
        }
    },
    {
        name: "retry on NotMaster with object change",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            let obj1 = {_id: 1, x: 5};
            let obj2 = {_id: 2, x: 5};
            assert.commandWorked(coll1.insert(obj1));
            assert.commandWorked(coll1.insert(obj2));
            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 5}]);
            obj1.x = 7;
            assert.commandWorked(coll1.update({_id: 2}, {$set: {x: 8}}));
            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 8}]);

            endCurrentTransactionIfOpen();
            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 8}]);
        }
    },
    {
        name: "implicit collection creation with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.NotMaster});
            let resDoc1 = coll1.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc1.a, 1);
            let resDoc2 = coll2.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2.a, 1);
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(
                ["create"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
            let resDoc1 = coll1.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc1.a, 1);
            let resDoc2 = coll2.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2.a, 1);
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal stepdown error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun(
                "create", ErrorCodes.NotMaster, "NotMaster", ErrorCodes.NotMaster, "NotMaster");
            let resDoc1 = coll1.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc1.a, 1);
            let resDoc2 = coll2.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2.a, 1);
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError and normal ordinary error",
        test: function() {
            failCommandWithErrorAndWCENoRun("create",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with ordinary error",
        test: function() {
            failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.OperationFailed});
            assert.throws(() => coll1.findAndModify(({update: {a: 1}, upsert: true})));
        }
    },
    {
        name: "implicit collection creation with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["create"], {closeConnection: true});
            let resDoc1 = coll1.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc1.a, 1);
            let resDoc2 = coll2.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2.a, 1);
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "implicit collection creation with WriteConcernError no success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithWCENoRun("create", ErrorCodes.NotMaster, "NotMaster");
            let resDoc1 = coll1.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc1.a, 1);
            let resDoc2 = coll2.findAndModify(({update: {a: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2.a, 1);
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "update with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with ordinary error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.OperationFailed});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandFailed(coll1.update({_id: 1}, {$inc: {x: 1}}));
        }
    },
    {
        name: "update with NoSuchTransaction error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NoSuchTransaction});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
        }
    },
    {
        name: "update with two stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"],
                                     {errorCode: ErrorCodes.NotMaster, mode: {times: 2}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "update with chained stepdown errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            // Chain multiple update errors together.
            attachPostCmdFunction("update", function() {
                failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.eq(coll1.find().toArray(), [{_id: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {y: 1}}));
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
        }
    },
    {
        name: "commit transaction with stepdown",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {errorCode: ErrorCodes.NotMaster});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with WriteConcernError",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(
                ["commitTransaction"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal stepdown error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.NotMaster,
                                            "NotMaster",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal ordinary error",
        test: function() {
            // We retry on write concern errors and this doesn't return OperationFailed again.
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal ordinary error twice",
        test: function() {
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.OperationFailed,
                                            "OperationFailed",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            // After commitTransaction fails, fail it again with just the ordinary error.
            attachPostCmdFunction("commitTransaction", function() {
                failCommandWithFailPoint(["commitTransaction"],
                                         {errorCode: ErrorCodes.OperationFailed});
            });
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with ordinary error",
        test: function() {
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.OperationFailed});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            assert.throws(() => endCurrentTransactionIfOpen());
        }
    },
    {
        name: "commit transaction with WriteConcernError and normal NoSuchTransaction error",
        test: function() {
            failCommandWithErrorAndWCENoRun("commitTransaction",
                                            ErrorCodes.NoSuchTransaction,
                                            "NoSuchTransaction",
                                            ErrorCodes.NotMaster,
                                            "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with NoSuchTransaction error",
        test: function() {
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.NoSuchTransaction});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with network error",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"], {closeConnection: true});
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commit transaction with WriteConcernError no success",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithWCENoRun("commitTransaction", ErrorCodes.NotMaster, "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1}));
            assert.commandWorked(coll2.insert({_id: 1}));
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);

            endCurrentTransactionIfOpen();
            assert.eq(coll1.find().itcount(), 1);
            assert.eq(coll2.find().itcount(), 1);
        }
    },
    {
        name: "commitTransaction fails with SERVER-38856",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(
                ["create"],
                {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});

            // After commitTransaction fails, abort the transaction and drop the collection
            // as if the transaction were being retried on a different node.
            attachPostCmdFunction("commitTransaction", function() {
                abortCurrentTransaction();
                assert.commandWorked(mongoRunCommandOriginal.apply(testDB.getMongo(),
                                                                   [dbName, {drop: collName2}, 0]));
            });
            failCommandWithWCENoRun("commitTransaction", ErrorCodes.NotMaster, "NotMaster");
            assert.commandWorked(coll1.insert({_id: 1, x: 2}));
            let resDoc2 = coll2.findAndModify(({update: {_id: 1}, upsert: true, 'new': true}));
            assert.eq(resDoc2._id, 1);
            assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 4}}));

            endCurrentTransactionIfOpen();

            assert.docEq(coll1.find().toArray(), [{_id: 1, x: 6}]);
            assert.docEq(coll2.find().toArray(), [resDoc2]);
        }
    },
    {
        name: 'Dates are copied correctly for SERVER-41917',
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.NoSuchTransaction});

            let date = new Date();
            assert.commandWorked(coll1.insert({_id: 3, a: date}));
            date.setMilliseconds(date.getMilliseconds() + 2);
            assert.eq(null, coll1.findOne({_id: 3, a: date}));
            const origDoc = coll1.findOne({_id: 3});
            const ret = assert.commandWorked(coll1.update({_id: 3}, {$min: {a: date}}));
            assert.eq(ret.nModified, 0);

            endCurrentTransactionIfOpen();

            assert.eq(coll1.findOne({_id: 3}).a, origDoc.a);
        }
    },
    {
        name: 'Timestamps are copied correctly for SERVER-41917',
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            failCommandWithFailPoint(["commitTransaction"],
                                     {errorCode: ErrorCodes.NoSuchTransaction});

            let ts = new Timestamp(5, 6);
            assert.commandWorked(coll1.insert({_id: 3, a: ts}));
            ts.t++;

            assert.eq(null, coll1.findOne({_id: 3, a: ts}));
            const origDoc = coll1.findOne({_id: 3});
            const ret = assert.commandWorked(coll1.update({_id: 3}, {$min: {a: ts}}));
            assert.eq(ret.nModified, 0);

            endCurrentTransactionIfOpen();

            assert.eq(coll1.findOne({_id: 3}).a, origDoc.a);
        }
    }
];

const retryOnReadErrorsFromBackgroundReconfigTest = [
    {
        name: "find retries on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            failCommandWithFailPoint(["find"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.eq(coll1.findOne({_id: 1}), {_id: 1});
        }
    },
    {
        name: "aggregate retries on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["aggregate"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            const cursor = coll1.aggregate([{$match: {a: 1}}]);
            assert.eq(cursor.toArray().length, 2);
        }
    },
    {
        name: "distinct retries on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["distinct"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.eq(coll1.distinct("a").sort(), [1, 2]);
        }
    },
    {
        name: "count retries on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["count"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.eq(coll1.count({a: 1}), 2);
        }
    },
];

const retryReadsOnNetworkErrorsWithNetworkRetryAndBackgroundReconfigTest = [
    {
        name: "find retries on network errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            failCommandWithFailPoint(["find"], {closeConnection: true});
            assert.eq(coll1.findOne({_id: 1}), {_id: 1});
        }
    },
    {
        name: "aggregate retries on network errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["aggregate"], {closeConnection: true});
            const cursor = coll1.aggregate([{$match: {a: 1}}]);
            assert.eq(cursor.toArray().length, 2);
        }
    },
    {
        name: "distinct retries on network errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["distinct"], {closeConnection: true});
            assert.eq(coll1.distinct("a").sort(), [1, 2]);
        }
    },
    {
        name: "count retries on network errors",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["count"], {closeConnection: true});
            assert.eq(coll1.count({a: 1}), 2);
        }
    },
];

const doNotRetryReadErrorWithOutBackgroundReconfigTest = [
    {
        name: "find fails on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({_id: 1}));
            failCommandWithFailPoint(["find"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.commandFailedWithCode(
                assert.throws(function() {
                                 coll1.findOne({_id: 1});
                             }),
                             ErrorCodes.ReadConcernMajorityNotAvailableYet);
        }
    },
    {
        name: "aggregate fails on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["aggregate"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.commandFailedWithCode(
                assert.throws(function() {
                                 const cursor = coll1.aggregate([{$match: {a: 1}}]);
                                 assert.eq(cursor.toArray().length, 2);
                             }),
                             ErrorCodes.ReadConcernMajorityNotAvailableYet);
        }
    },
    {
        name: "distinct fails on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["distinct"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.commandFailedWithCode(
                assert.throws(function() {
                                 coll1.distinct("a");
                             }),
                             ErrorCodes.ReadConcernMajorityNotAvailableYet);
        }
    },
    {
        name: "count fails on ReadConcernMajorityNotAvailableYet",
        test: function() {
            assert.commandWorked(testDB.createCollection(collName1));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 1}));
            assert.commandWorked(coll1.insert({a: 2}));
            failCommandWithFailPoint(["count"],
                                     {errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet});
            assert.commandFailedWithCode(
                assert.throws(function() {
                                 coll1.count({a: 1});
                             }),
                             ErrorCodes.ReadConcernMajorityNotAvailableYet);
        }
    },
];

TestData.networkErrorAndTxnOverrideConfig = {};
TestData.sessionOptions = new SessionOptions();
TestData.overrideRetryAttempts = 3;

let session = conn.startSession(TestData.sessionOptions);
let testDB = session.getDatabase(dbName);

load("jstests/libs/override_methods/network_error_and_txn_override.js");

jsTestLog("=-=-=-=-=-= Testing with 'retry on network error' by itself. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: true});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = true;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = false;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
let coll1 = testDB[collName1];
let coll2 = testDB[collName2];

retryOnNetworkErrorTests.forEach((testCase) => runTest("retryOnNetworkErrorTests", testCase));

jsTestLog("=-=-=-=-=-= Testing with 'txn override' by itself. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: false});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = false;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = true;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
coll1 = testDB[collName1];
coll2 = testDB[collName2];

txnOverrideTests.forEach((testCase) => runTest("txnOverrideTests", testCase));

jsTestLog("=-=-=-=-=-= Testing 'both txn override and retry on network error'. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: true});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = true;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = true;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
coll1 = testDB[collName1];
coll2 = testDB[collName2];

txnOverridePlusRetryOnNetworkErrorTests.forEach(
    (testCase) => runTest("txnOverridePlusRetryOnNetworkErrorTests", testCase));

jsTestLog("=-=-=-=-=-= Testing 'retry on read errors from background reconfigs'. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: false});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = false;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = true;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
coll1 = testDB[collName1];
coll2 = testDB[collName2];

retryOnReadErrorsFromBackgroundReconfigTest.forEach(
    (testCase) => runTest("retryOnReadErrorsFromBackgroundReconfigTest", testCase));

jsTestLog(
    "=-=-=-=-=-= Testing 'retry on network errors during network error retry and background reconfigs'. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: true});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = true;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = true;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
coll1 = testDB[collName1];
coll2 = testDB[collName2];

retryReadsOnNetworkErrorsWithNetworkRetryAndBackgroundReconfigTest.forEach(
    (testCase) =>
        runTest("retryReadsOnNetworkErrorsWithNetworkRetryAndBackgroundReconfigTest", testCase));

jsTestLog(
    "=-=-=-=-=-= Testing 'don't retry on network errors during background reconfigs'. =-=-=-=-=-=");
TestData.sessionOptions = new SessionOptions({retryWrites: true});
TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors = true;
TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs = false;
TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions = false;

session = conn.startSession(TestData.sessionOptions);
testDB = session.getDatabase(dbName);
coll1 = testDB[collName1];
coll2 = testDB[collName2];

doNotRetryReadErrorWithOutBackgroundReconfigTest.forEach(
    (testCase) => runTest("doNotRetryReadErrorWithOutBackgroundReconfigTest", testCase));

rst.stopSet();
})();
