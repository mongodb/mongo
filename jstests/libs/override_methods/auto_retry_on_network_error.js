/**
 * When a network connection to the mongo shell is closed, attempting to call
 * Mongo.prototype.runCommand() and Mongo.prototype.runCommandWithMetadata() throws a JavaScript
 * exception. This override catches these exceptions (i.e. ones where isNetworkError() returns true)
 * and automatically re-sends the command request to the server, or propagates the error if the
 * command should already be using the shell's existing retryability logic. The goal of this
 * override is to implement retry logic such that the assertions within our existing JavaScript
 * tests still pass despite stepdowns of the CSRS and replica set shards happening in the
 * background.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");
    load("jstests/libs/retryable_writes_util.js");

    const kMaxNumRetries = 3;

    // Store a session to access ServerSession#canRetryWrites.
    let _serverSession;

    const mongoRunCommandOriginal = Mongo.prototype.runCommand;
    const mongoRunCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;

    Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
        if (typeof _serverSession === "undefined") {
            _serverSession = this.startSession()._serverSession;
        }

        return runWithRetriesOnNetworkErrors(this, cmdObj, mongoRunCommandOriginal, arguments);
    };

    Mongo.prototype.runCommandWithMetadata = function runCommandWithMetadata(
        dbName, metadata, cmdObj) {
        if (typeof _serverSession === "undefined") {
            _serverSession = this.startSession()._serverSession;
        }

        return runWithRetriesOnNetworkErrors(
            this, cmdObj, mongoRunCommandWithMetadataOriginal, arguments);
    };

    // Commands assumed to not be blindly retryable.
    const kNonRetryableCommands = new Set([
        // Commands that take write concern and do not support txnNumbers.
        "_configsvrAddShard",
        "_configsvrAddShardToZone",
        "_configsvrCommitChunkMerge",
        "_configsvrCommitChunkMigration",
        "_configsvrCommitChunkSplit",
        "_configsvrCreateDatabase",
        "_configsvrEnableSharding",
        "_configsvrMoveChunk",
        "_configsvrMovePrimary",
        "_configsvrRemoveShard",
        "_configsvrRemoveShardFromZone",
        "_configsvrShardCollection",
        "_configsvrUpdateZoneKeyRange",
        "_mergeAuthzCollections",
        "_recvChunkStart",
        "appendOplogNote",
        "applyOps",
        "authSchemaUpgrade",
        "captrunc",
        "cleanupOrphaned",
        "clone",
        "cloneCollection",
        "cloneCollectionAsCapped",
        "collMod",
        "convertToCapped",
        "copydb",
        "create",
        "createIndexes",
        "createRole",
        "createUser",
        "deleteIndexes",
        "drop",
        "dropAllRolesFromDatabase",
        "dropAllUsersFromDatabase",
        "dropDatabase",
        "dropIndexes",
        "dropRole",
        "dropUser",
        "emptycapped",
        "godinsert",
        "grantPrivilegesToRole",
        "grantRolesToRole",
        "grantRolesToUser",
        "mapreduce.shardedfinish",
        "moveChunk",
        "renameCollection",
        "revokePrivilegesFromRole",
        "revokeRolesFromRole",
        "revokeRolesFromUser",
        "updateRole",
        "updateUser",

        // Other commands.
        "eval",  // May contain non-retryable commands.
        "$eval",
    ]);

    // These commands are not idempotent because they return errors if retried after
    // successfully completing (like IndexNotFound, NamespaceExists, etc.), but because they
    // only take effect once, and many tests use them to set up state, their errors on retries
    // are handled specially.
    const kAcceptableNonRetryableCommands = new Set([
        "create",
        "createIndexes",
        "deleteIndexes",
        "drop",
        "dropDatabase",  // Already ignores NamespaceNotFound errors, so not handled below.
        "dropIndexes",
    ]);

    function isAcceptableNonRetryableCommand(cmdName) {
        return kAcceptableNonRetryableCommands.has(cmdName);
    }

    function isAcceptableRetryFailedResponse(cmdName, res) {
        return ((cmdName === "create" && res.code === ErrorCodes.NamespaceExists) ||
                (cmdName === "createIndexes" && res.code === ErrorCodes.IndexAlreadyExists) ||
                (cmdName === "drop" && res.code === ErrorCodes.NamespaceNotFound) ||
                ((cmdName === "dropIndexes" || cmdName === "deleteIndexes") &&
                 res.code === ErrorCodes.IndexNotFound));
    }

    // Commands that may return different values or fail if retried on a new primary after a
    // failover.
    const kNonFailoverTolerantCommands = new Set([
        "currentOp",  // Failovers can change currentOp output.
        "getLog",     // The log is different on different servers.
        "killOp",     // Failovers may interrupt operations intended to be killed later in the test.
        "logRotate",
        "planCacheClear",  // The plan cache isn't replicated.
        "planCacheClearFilters",
        "planCacheListFilters",
        "planCacheListPlans",
        "planCacheListQueryShapes",
        "planCacheSetFilter",
        "profile",       // Not replicated, so can't tolerate failovers.
        "setParameter",  // Not replicated, so can't tolerate failovers.
        "stageDebug",
        "startSession",  // Sessions are flushed to disk asynchronously.
    ]);

    // Several commands that use the plan executor swallow the actual error code from a failed plan
    // into their error message and instead return OperationFailed.
    //
    // TODO SERVER-32208: Remove this function once it is no longer needed.
    function isRetryableExecutorCodeAndMessage(code, msg) {
        return code === ErrorCodes.OperationFailed && typeof msg !== "undefined" &&
            msg.indexOf("InterruptedDueToReplStateChange") >= 0;
    }

    function runWithRetriesOnNetworkErrors(mongo, cmdObj, clientFunction, clientFunctionArguments) {
        let cmdName = Object.keys(cmdObj)[0];

        // If the command is in a wrapped form, then we look for the actual command object
        // inside the query/$query object.
        if (cmdName === "query" || cmdName === "$query") {
            cmdObj = cmdObj[cmdName];
            cmdName = Object.keys(cmdObj)[0];
        }

        const isRetryableWriteCmd = RetryableWritesUtil.isRetryableWriteCmdName(cmdName);
        const canRetryWrites = _serverSession.canRetryWrites(cmdObj);

        const startTime = Date.now();
        let numRetries = !jsTest.options().skipRetryOnNetworkError ? kMaxNumRetries : 0;

        // Validate the command before running it, to prevent tests with non-retryable commands
        // from being run.
        if (isRetryableWriteCmd && !canRetryWrites) {
            throw new Error("Refusing to run a test that issues non-retryable write operations" +
                            " since the test likely makes assertions on the write results and" +
                            " can lead to spurious failures if a network error occurs.");
        } else if (cmdName === "getMore") {
            throw new Error(
                "Refusing to run a test that issues a getMore command since if a network error" +
                " occurs during it then we won't know whether the cursor was advanced or not.");
        } else if (kNonRetryableCommands.has(cmdName) &&
                   !isAcceptableNonRetryableCommand(cmdName)) {
            throw new Error(
                "Refusing to run a test that issues commands that are not blindly retryable, " +
                " cmdName: " + cmdName);
        } else if (kNonFailoverTolerantCommands.has(cmdName)) {
            throw new Error(
                "Refusing to run a test that issues commands that may return different values" +
                " after a failover, cmdName: " + cmdName);
        } else if (cmdName === "aggregate") {
            var stages = cmdObj.pipeline;

            // $listLocalCursors and $listLocalSessions must be the first stage in the pipeline.
            const firstStage =
                stages && Array.isArray(stages) && (stages.length > 0) ? stages[0] : undefined;
            const hasListLocalStage = firstStage && (typeof firstStage === "object") &&
                (firstStage.hasOwnProperty("$listLocalCursors") ||
                 firstStage.hasOwnProperty("$listLocalSessions"));
            if (hasListLocalStage) {
                throw new Error(
                    "Refusing to run a test that issues an aggregation command with" +
                    " $listLocalCursors or $listLocalSessions because they rely on in-memory" +
                    " state that may not survive failovers.");
            }

            // Aggregate can be either a read or a write depending on whether it has a $out stage.
            // $out is required to be the last stage of the pipeline.
            const lastStage = stages && Array.isArray(stages) && (stages.length !== 0)
                ? stages[stages.length - 1]
                : undefined;
            const hasOut =
                lastStage && (typeof lastStage === "object") && lastStage.hasOwnProperty("$out");
            if (hasOut) {
                throw new Error("Refusing to run a test that issues an aggregation command" +
                                " with $out because it is not retryable.");
            }

            const hasExplain = cmdObj.hasOwnProperty("explain");
            if (hasExplain) {
                throw new Error(
                    "Refusing to run a test that issues an aggregation command with explain" +
                    " because it may return incomplete results if interrupted by a stepdown.");
            }
        } else if (cmdName === "mapReduce" || cmdName === "mapreduce") {
            throw new Error(
                "Refusing to run a test that issues a mapReduce command, because it calls " +
                " std::terminate() if interrupted by a stepdown.");
        }

        do {
            try {
                let res = clientFunction.apply(mongo, clientFunctionArguments);

                if (isRetryableWriteCmd) {
                    // findAndModify can fail during the find stage and return an executor error.
                    if ((cmdName === "findandmodify" || cmdName === "findAndModify") &&
                        isRetryableExecutorCodeAndMessage(res.code, res.errmsg)) {
                        print("=-=-=-= Retrying because of executor interruption: " + cmdName +
                              ", retries remaining: " + numRetries);
                        continue;
                    }

                    // Don't interfere with retryable writes.
                    return res;
                }

                if (cmdName === "explain") {
                    // If an explain is interrupted by a stepdown, and it returns before its
                    // connection is closed, it will return incomplete results. To prevent failing
                    // the test, force retries of interrupted explains.
                    if (res.hasOwnProperty("executionStats") &&
                        !res.executionStats.executionSuccess &&
                        (RetryableWritesUtil.isRetryableCode(res.executionStats.errorCode) ||
                         isRetryableExecutorCodeAndMessage(res.executionStats.errorCode,
                                                           res.executionStats.errorMessage))) {
                        print("=-=-=-= Forcing retry of interrupted explain, res: " + tojson(res));
                        continue;
                    }

                    // An explain command can fail if its child command cannot be run on the current
                    // server. This can be hit if a primary only or not explicitly slaveOk command
                    // is accepted by a primary node that then steps down and returns before having
                    // its connection closed.
                    if (!res.ok &&
                        res.errmsg.indexOf("child command cannot run on this node") >= 0) {
                        print(
                            "=-=-=-= Forcing retry of explain likely interrupted by transition to" +
                            " secondary, res: " + tojson(res));
                        continue;
                    }
                }

                if (!res.ok) {
                    if (numRetries > 0) {
                        if (RetryableWritesUtil.isRetryableCode(res.code)) {
                            // Don't decrement retries, because the command returned before the
                            // connection was closed, so a subsequent attempt will receive a
                            // network error (or NotMaster error) and need to retry.
                            print("=-=-=-= Retrying failed response with retryable code: " +
                                  res.code + ", for command: " + cmdName + ", retries remaining: " +
                                  numRetries);
                            continue;
                        }

                        if (isRetryableExecutorCodeAndMessage(res.code, res.errmsg)) {
                            print("=-=-=-= Retrying because of executor interruption: " + cmdName +
                                  ", retries remaining: " + numRetries);
                            continue;
                        }

                        // listCollections and listIndexes called through mongos may return
                        // OperationFailed if the request to establish a cursor on the targeted
                        // shard fails with a network error.
                        //
                        // TODO SERVER-30949: Remove this check once those two commands retry on
                        // retryable errors automatically.
                        if ((cmdName === "listCollections" || cmdName === "listIndexes") &&
                            res.code === ErrorCodes.OperationFailed &&
                            res.hasOwnProperty("errmsg") &&
                            res.errmsg.indexOf("failed to read command response from shard") >= 0) {
                            print("=-=-=-= Retrying failed mongos cursor command: " + cmdName +
                                  ", retries remaining: " + numRetries);
                            continue;
                        }

                        // Thrown when an index build is interrupted during its collection scan.
                        if ((cmdName === "createIndexes" && res.code === 28550)) {
                            print("=-=-=-= Retrying because of interrupted collection scan: " +
                                  cmdName + ", retries remaining: " + numRetries);
                            continue;
                        }
                    }

                    // Swallow safe errors that may come from a retry since the command may have
                    // completed before the connection was closed.
                    if (isAcceptableRetryFailedResponse(cmdName, res)) {
                        print("=-=-=-= Overriding safe failed response for: " + cmdName +
                              ", code: " + res.code + ", retries remaining: " + numRetries);
                        res.ok = 1;
                    }
                }

                if (res.writeConcernError && numRetries > 0) {
                    if (RetryableWritesUtil.isRetryableCode(res.writeConcernError.code)) {
                        // Don't decrement retries, because the command returned before the
                        // connection was closed, so a subsequent attempt will receive a
                        // network error (or NotMaster error) and need to retry.
                        print("=-=-=-= Retrying write concern error with retryable code: " +
                              res.writeConcernError.code + ", for command: " + cmdName +
                              ", retries remaining: " + numRetries);
                        continue;
                    }
                }

                return res;
            } catch (e) {
                const kReplicaSetMonitorError =
                    /^Could not find host matching read preference.*mode: "primary"/;

                if (numRetries === 0) {
                    throw e;
                } else if (e.message.match(kReplicaSetMonitorError) &&
                           Date.now() - startTime < 5 * 60 * 1000) {
                    // ReplicaSetMonitor::getHostOrRefresh() waits up to 15 seconds to find the
                    // primary of the replica set. It is possible for the step up attempt of another
                    // node in the replica set to take longer than 15 seconds so we allow retrying
                    // for up to 5 minutes.
                    print("=-=-=-= Failed to find primary when attempting to run " + cmdName +
                          " command, will retry for another 15 seconds");
                    continue;
                } else if (!isNetworkError(e)) {
                    throw e;
                } else if (isRetryableWriteCmd) {
                    if (canRetryWrites) {
                        // If the command is retryable, assume the command has already gone through
                        // or will go through the retry logic in SessionAwareClient, so propagate
                        // the error.
                        throw e;
                    }
                }

                --numRetries;
                print("=-=-=-= Retrying on network error for command: " + cmdName +
                      ", retries remaining: " + numRetries);
            }
        } while (numRetries >= 0);
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/auto_retry_on_network_error.js");

    const connectOriginal = connect;

    connect = function(url, user, pass) {
        let retVal;

        let connectionAttempts = 0;
        assert.soon(
            () => {
                try {
                    connectionAttempts += 1;
                    retVal = connectOriginal.apply(this, arguments);
                    return true;
                } catch (e) {
                    print("=-=-=-= Retrying connection to: " + url + ", attempts: " +
                          connectionAttempts + ", failed with: " + tojson(e));
                }
            },
            "Failed connecting to url: " + tojson(url),
            undefined,  // Default timeout.
            2000);      // 2 second interval.

        return retVal;
    };
})();
