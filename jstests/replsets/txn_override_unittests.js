/**
 * Verifies the txn_override passthrough behaves correctly. txn_override groups sequential CRUD
 * commands into transactions and commits them when a DDL command arrives. When used with
 * auto_retry_on_network_error, it retries transactions on network errors. This tests that errors
 * are surfaced correctly and retried correctly in various error cases.
 *
 * Commands are set to fail in two ways:
 *     1. 'failCommand' failpoint
 *        Commands that fail with this failpoint generally fail before running the command, with the
 *        exception of WriteConcernErrors which occur after the command has run. This also allows us
 *        to close the connection on the server and simulate network errors.
 *
 *     2. 'runCommand' override
 *        This completely mocks out responses from the server without running a command at all. This
 *        is mainly used for generating a WriteConcernError without running the command at all,
 *        simulating a WriteConcernError on a command that later gets rolled back.
 *
 * The test also makes use of "post-command functions" for injecting functionality at times when
 * the test is not given control. Since txn_override.js may run multiple commands when instructed to
 * run a single command (for retries for example), it is important to be able to run functions in
 * between those extra commands. To do so, we add a hook through the 'runCommand' override that runs
 * a given function after a specific command.
 *
 * TODO(SERVER-38937): Many of these tests assert that a command fails but do not care with which
 * code. This is intentional since it doesn't really matter how the test fails as long as it fails
 * and this allows us to keep more tests running now. That said, these should ideally all throw so
 * we do not rely on the test itself calling assert.commandWorked. In SERVER-38937 we should
 * convert all assert.commandFailed assertions to assert.throws.
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
     * Sets that the given command should fail with ok:1 and the given write concern error.
     * The command will not actually be run.
     */
    function failCommandWithWCENoRun(cmdName, writeConcernErrorCode, writeConcernErrorCodeName) {
        assert(!runCommandOverrideBlacklistedCommands.includes(cmdName));

        cmdResponseOverrides[cmdName] = {
            responseObj: {
                ok: 1,
                writeConcernError:
                    {code: writeConcernErrorCode, codeName: writeConcernErrorCodeName}
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
                writeConcernError:
                    {code: writeConcernErrorCode, codeName: writeConcernErrorCodeName}
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
            postCommandFuncs[cmdName]();
            clearPostCommandFunc(cmdName);
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
    const testDB = conn.getDB(dbName);
    testDB.setLogLevel(2, 'command');

    const coll1 = testDB[collName1];
    const coll2 = testDB[collName2];

    // We have a separate connection for the failpoint so that it does not break up the transaction
    // buffered in txn_override.js.
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
        mode: mode = {times: 1},
    } = {}) {
        // The fail point will ignore the WCE if an error code is specified.
        assert(!(writeConcernError && errorCode),
               "Cannot specify both a WCE " + tojsononeline(writeConcernError) +
                   " and an error code " + errorCode);

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
            failpointConn,
            ['admin', {configureFailPoint: "failCommand", mode: mode, data: data}, 0]));
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
     * txn_override.js to commit the current transaction in order to run the 'ping'.
     */
    function endCurrentTransactionIfOpen() {
        assert.commandWorked(testDB.runCommand({ping: 1}));
    }

    /**
     * Aborts the current transaction in txn_override.js.
     */
    function abortCurrentTransaction() {
        const session = testDB.getSession();
        assert.commandWorked(mongoRunCommandOriginal.apply(testDB.getMongo(), [
            'admin',
            {
              abortTransaction: 1,
              autocommit: false,
              lsid: session.getSessionId(),
              txnNumber: session.getTxnNumber_forTesting()
            },
            0
        ]));
    }

    /**
     * Runs a test where a transaction attempts to use a forbidden database name. When running a
     * CRUD operation on one of these databases, txn_override.js is expected to commit the current
     * transaction and run the CRUD operation outside of a transaction.
     */
    function testBadDBName(badDBName) {
        const badDB = conn.getDB(badDBName);
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
    function runTest(testCase) {
        coll1.drop();
        coll2.drop();

        // Ensure all overrides and failpoints have been turned off before running the test.
        clearAllCommandOverrides();
        stopFailingCommands();

        jsTestLog("Testing " + testCase.name);
        testCase.test();
        jsTestLog("Test " + testCase.name + " complete.");

        // End the current transaction if the test did not end it itself.
        endCurrentTransactionIfOpen();

        // Ensure all overrides and failpoints have been turned off after running the test as well.
        clearAllCommandOverrides();
        stopFailingCommands();
    }

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
          name: "implicit collection creation",
          test: function() {
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().itcount(), 1);

              endCurrentTransactionIfOpen();
              assert.eq(coll1.find().itcount(), 1);
          }
        },
        {
          name: "errors cause transaction to abort",
          test: function() {
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
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
              assert.commandFailed(coll1.update({_id: 1}, {$set: {x: 1}}));
          }
        },
        {
          name: "update with NoSuchTransaction error",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NoSuchTransaction});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().toArray(), [{_id: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
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
              assert.throws(() => coll1.update({_id: 1}, {$set: {x: 1}}));

              endCurrentTransactionIfOpen();
              assert.eq(coll1.find().toArray(), [{_id: 1}]);
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {y: 1}}));
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {y: 1}}));
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);

              endCurrentTransactionIfOpen();
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1, y: 1}]);
          }
        },
        // TODO(SERVER-38937): this command should fail.
        // {
        //   name: "implicit collection creation with stepdown",
        //   test: function() {
        //       failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.NotMaster});
        //       assert.commandFailed(coll1.insert({_id: 1}));
        //   }
        // },
        {
          name: "implicit collection creation with WriteConcernError",
          test: function() {
              failCommandWithFailPoint(
                  ["create"],
                  {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
              assert.throws(() => coll1.insert({_id: 1}));
          }
        },
        // TODO(SERVER-38937): this command should fail.
        // {
        //   name: "implicit collection creation with WriteConcernError and normal stepdown error",
        //   test: function() {
        //       failCommandWithErrorAndWCENoRun(
        //           "create", ErrorCodes.NotMaster, "NotMaster", ErrorCodes.NotMaster,
        //           "NotMaster");
        //       assert.commandFailed(coll1.insert({_id: 1}));
        //   }
        // },
        {
          name: "implicit collection creation with WriteConcernError and normal ordinary error",
          test: function() {
              failCommandWithErrorAndWCENoRun("create",
                                              ErrorCodes.OperationFailed,
                                              "OperationFailed",
                                              ErrorCodes.NotMaster,
                                              "NotMaster");
              assert.commandFailed(coll1.insert({_id: 1}));
          }
        },
        {
          name: "implicit collection creation with ordinary error",
          test: function() {
              failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.OperationFailed});
              assert.commandFailed(coll1.insert({_id: 1}));
          }
        },
        {
          name: "implicit collection creation with network error",
          test: function() {
              failCommandWithFailPoint(["create"], {closeConnection: true});
              assert.throws(() => coll1.insert({_id: 1}));
          }
        },
        {
          name: "implicit collection creation with WriteConcernError no success",
          test: function() {
              failCommandWithWCENoRun("create", ErrorCodes.NotMaster, "NotMaster");
              assert.throws(() => coll1.insert({_id: 1}));
          }
        },
        {
          name: "errors cause the override to abort transactions",
          test: function() {
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
              failCommandWithFailPoint(["commitTransaction"], {errorCode: ErrorCodes.NotMaster});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().itcount(), 1);
              assert.throws(() => endCurrentTransactionIfOpen());
          }
        },
        // TODO(SERVER-38937): this command should throw.
        // {
        //   name: "commit transaction with WriteConcernError",
        //   test: function() {
        //       failCommandWithFailPoint(
        //           ["commitTransaction"],
        //           {writeConcernError: {code: ErrorCodes.NotMaster, codeName: "NotMaster"}});
        //       assert.commandWorked(coll1.insert({_id: 1}));
        //       assert.eq(coll1.find().itcount(), 1);
        //       assert.throws(() => endCurrentTransactionIfOpen());
        //   }
        // },
        // TODO(SERVER-38937): this command should throw.
        // {
        //   name: "commit transaction with WriteConcernError and normal stepdown error",
        //   test: function() {
        //       failCommandWithErrorAndWCENoRun("commitTransaction",
        //                                           ErrorCodes.NotMaster,
        //                                           "NotMaster",
        //                                           ErrorCodes.NotMaster,
        //                                           "NotMaster");
        //       assert.commandWorked(coll1.insert({_id: 1}));
        //       assert.eq(coll1.find().itcount(), 1);
        //       assert.throws(() => endCurrentTransactionIfOpen());
        //   }
        // },
        // TODO(SERVER-38937): this command should throw.
        // {
        //   name: "commit transaction with WriteConcernError and normal ordinary error",
        //   test: function() {
        //       failCommandWithErrorAndWCENoRun("commitTransaction",
        //                                           ErrorCodes.OperationFailed,
        //                                           "OperationFailed",
        //                                           ErrorCodes.NotMaster,
        //                                           "NotMaster");
        //       assert.commandWorked(coll1.insert({_id: 1}));
        //       assert.eq(coll1.find().itcount(), 1);
        //       assert.throws(() => endCurrentTransactionIfOpen());
        //   }
        // },
        {
          name: "commit transaction with ordinary error",
          test: function() {
              failCommandWithFailPoint(["commitTransaction"],
                                       {errorCode: ErrorCodes.OperationFailed});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().itcount(), 1);
              assert.throws(() => endCurrentTransactionIfOpen());
          }
        },
        // TODO(SERVER-38937): this command should throw.
        // {
        //   name: "commit transaction with WriteConcernError and normal NoSuchTransaction error",
        //   test: function() {
        //       failCommandWithErrorAndWCENoRun("commitTransaction",
        //                                           ErrorCodes.NoSuchTransaction,
        //                                           "NoSuchTransaction",
        //                                           ErrorCodes.NotMaster,
        //                                           "NotMaster");
        //       assert.commandWorked(coll1.insert({_id: 1}));
        //       assert.eq(coll1.find().itcount(), 1);
        //       assert.throws(() => endCurrentTransactionIfOpen());
        //   }
        // },
        {
          name: "commit transaction with NoSuchTransaction error",
          test: function() {
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
              failCommandWithFailPoint(["commitTransaction"], {closeConnection: true});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().itcount(), 1);
              assert.throws(() => endCurrentTransactionIfOpen());
          }
        },
        // TODO(SERVER-38937): this command should throw.
        // {
        //   name: "commit transaction with WriteConcernError no success",
        //   test: function() {
        //       failCommandWithWCENoRun(
        //           "commitTransaction", ErrorCodes.NotMaster, "NotMaster");
        //       assert.commandWorked(coll1.insert({_id: 1}));
        //       assert.eq(coll1.find().itcount(), 1);
        //       assert.throws(() => endCurrentTransactionIfOpen());
        //   }
        // },
        {
          name: "commands in 'admin' database end transaction",
          test: function() {
              testBadDBName('admin');
          }
        },
        {
          name: "commands in 'config' database end transaction",
          test: function() {
              testBadDBName('config');
          }
        },
        {
          name: "commands in 'local' database end transaction",
          test: function() {
              testBadDBName('local');
          }
        },
    ];

    // Failpoints, overrides, and post-command functions are set by default to only run once, so
    // commands should succeed on retry.
    const txnOverridePlusRetryOnNetworkErrorTests = [
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
        // TODO(SERVER-38937): this test should pass.
        // {
        //   name: "retry on NotMaster with object change",
        //   test: function() {
        //       assert.commandWorked(testDB.createCollection(collName1));
        //       failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
        //       let obj1 = {_id: 1, x: 5};
        //       let obj2 = {_id: 2, x: 5};
        //       assert.commandWorked(coll1.insert(obj1));
        //       assert.commandWorked(coll1.insert(obj2));
        //       assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 5}]);
        //       obj1.x = 7
        //       assert.commandWorked(coll1.update({_id: 2}, {$set: {x: 8}}));
        //       assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 8}]);
        //
        //       endCurrentTransactionIfOpen();
        //       assert.docEq(coll1.find().toArray(), [{_id: 1, x: 5}, {_id: 2, x: 8}]);
        //   }
        // },
        {
          name: "implicit collection creation with stepdown",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.NotMaster});
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
          name: "implicit collection creation with WriteConcernError",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(
                  ["create"],
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
          name: "implicit collection creation with WriteConcernError and normal stepdown error",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithErrorAndWCENoRun(
                  "create", ErrorCodes.NotMaster, "NotMaster", ErrorCodes.NotMaster, "NotMaster");
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
          name: "implicit collection creation with WriteConcernError and normal ordinary error",
          test: function() {
              failCommandWithErrorAndWCENoRun("create",
                                              ErrorCodes.OperationFailed,
                                              "OperationFailed",
                                              ErrorCodes.NotMaster,
                                              "NotMaster");
              assert.commandFailed(coll1.insert({_id: 1}));
          }
        },
        {
          name: "implicit collection creation with ordinary error",
          test: function() {
              failCommandWithFailPoint(["create"], {errorCode: ErrorCodes.OperationFailed});
              assert.commandFailed(coll1.insert({_id: 1}));
          }
        },
        {
          name: "implicit collection creation with network error",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(["create"], {closeConnection: true});
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
          name: "implicit collection creation with WriteConcernError no success",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithWCENoRun("create", ErrorCodes.NotMaster, "NotMaster");
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
          name: "update with stepdown",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NotMaster});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().toArray(), [{_id: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
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
              assert.commandFailed(coll1.update({_id: 1}, {$set: {x: 1}}));
          }
        },
        {
          name: "update with NoSuchTransaction error",
          test: function() {
              assert.commandWorked(testDB.createCollection(collName1));
              failCommandWithFailPoint(["update"], {errorCode: ErrorCodes.NoSuchTransaction});
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.eq(coll1.find().toArray(), [{_id: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {y: 1}}));
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
              assert.commandWorked(coll1.update({_id: 1}, {$set: {x: 1}}));
              assert.eq(coll1.find().toArray(), [{_id: 1, x: 1}]);
              assert.commandWorked(coll1.update({_id: 1}, {$set: {y: 1}}));
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
              failCommandWithErrorAndWCENoRun("commitTransaction",
                                              ErrorCodes.OperationFailed,
                                              "OperationFailed",
                                              ErrorCodes.NotMaster,
                                              "NotMaster");
              assert.commandWorked(coll1.insert({_id: 1}));
              assert.commandWorked(coll2.insert({_id: 1}));
              assert.eq(coll1.find().itcount(), 1);
              assert.eq(coll2.find().itcount(), 1);

              // 'commitTransaction' only fails the first time and is retried due to the WCE.
              endCurrentTransactionIfOpen();
              assert.eq(coll1.find().itcount(), 1);
              assert.eq(coll2.find().itcount(), 1);
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

              // 'commitTransaction' only fails the first time and is retried due to the WCE.
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
        // TODO(SERVER-38937): fix this test case and uncomment.
        // {
        //   name: "commitTransaction fails with SERVER-38856",
        //   test: function() {
        //       assert.commandWorked(testDB.createCollection(collName1));
        //       failCommandWithFailPoint(["create"],
        //                   {writeConcernError: {code: ErrorCodes.NotMaster, codeName:
        //                   "NotMaster"}});
        //
        //       // After commitTransaction fails, abort the transaction and drop the collection
        //       // as if the transaction were being retried on a different node.
        //       attachPostCmdFunction("commitTransaction", function() {
        //           abortCurrentTransaction();
        //           assert.commandWorked(mongoRunCommandOriginal.apply(
        //               testDB.getMongo(), [dbName, {drop: collName2}, 0]));
        //       });
        //       failCommandWithWCENoRun("commitTransaction", ErrorCodes.NotMaster,
        //       "NotMaster");
        //       assert.commandWorked(coll1.insert({_id: 1, x: 2}));
        //       assert.commandWorked(coll2.insert({_id: 2}));
        //       assert.commandWorked(coll1.update({_id: 1}, {$inc: {x: 4}}));
        //
        //       endCurrentTransactionIfOpen();
        //
        //       assert.docEq(coll1.find().toArray(), [{_id: 1, x: 6}]);
        //       assert.docEq(coll2.find().toArray(), [{_id: 2}]);
        //   }
        // },
    ];

    jsTestLog("=-=-=-=-=-= Testing with 'txn override' by itself. =-=-=-=-=-=");
    load("jstests/libs/txns/txn_override.js");
    txnOverrideTests.forEach(runTest);

    jsTestLog("=-=-=-=-=-= Testing with 'auto retry on network error'. =-=-=-=-=-=");
    load("jstests/libs/override_methods/auto_retry_on_network_error.js");
    txnOverridePlusRetryOnNetworkErrorTests.forEach(runTest);

    rst.stopSet();
})();
