/**
 * Use prototype overrides to set read concern and write concern while running tests.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    if (typeof TestData === "undefined" || !TestData.hasOwnProperty("defaultReadConcernLevel")) {
        throw new Error(
            "The readConcern level to use must be set as the 'defaultReadConcernLevel'" +
            " property on the global TestData object");
    }

    const kDefaultReadConcern = {
        level: TestData.defaultReadConcernLevel
    };
    const kDefaultWriteConcern =
        (TestData.hasOwnProperty("defaultWriteConcern")) ? TestData.defaultWriteConcern : {
            w: "majority",
            // Use a "signature" value that won't typically match a value assigned in normal use.
            // This way the wtimeout set by this override is distinguishable in the server logs.
            wtimeout: 5 * 60 * 1000 + 321,  // 300321ms
        };

    const kCommandsSupportingReadConcern = new Set([
        "aggregate",
        "count",
        "distinct",
        "find",
        "geoNear",
        "geoSearch",
        "group",
        "parallelCollectionScan",
    ]);

    const kCommandsSupportingWriteConcern = new Set([
        "applyOps",
        "authSchemaUpgrade",
        "createRole",
        "createUser",
        "delete",
        "dropAllRolesFromDatabase",
        "dropAllUsersFromDatabase",
        "dropRole",
        "dropUser",
        "findAndModify",
        "findandmodify",
        "grantPrivilegesToRole",
        "grantRolesToRole",
        "grantRolesToUser",
        "insert",
        "revokeRolesFromRole",
        "revokeRolesFromUser",
        "update",
        "updateRole",
        "updateUser",
    ]);

    const kCommandsToEmulateWriteConcern = new Set([
        "aggregate",
        "appendOplogNote",
        "captrunc",
        "cleanupOrphaned",
        "clone",
        "cloneCollection",
        "cloneCollectionAsCapped",
        "convertToCapped",
        "copydb",
        "create",
        "createIndexes",
        "deleteIndexes",
        "drop",
        "dropDatabase",
        "dropIndexes",
        "emptycapped",
        "godinsert",
        "mapReduce",
        "mapreduce",
        "mapreduce.shardedfinish",
        "moveChunk",
        "renameCollection",
        "revokePrivilegesFromRole",
    ]);

    function runCommandWithReadAndWriteConcerns(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        var commandObjUnwrapped = commandObj;
        if (commandName === "query" || commandName === "$query") {
            commandObjUnwrapped = commandObj[commandName];
            commandName = Object.keys(commandObjUnwrapped)[0];
        }

        if (commandName === "collMod" || commandName === "eval" || commandName === "$eval") {
            throw new Error("Cowardly refusing to run test with overridden write concern when it" +
                            " uses a command that can only perform w=1 writes: " +
                            tojson(commandObj));
        }

        var shouldForceReadConcern = kCommandsSupportingReadConcern.has(commandName);
        var shouldForceWriteConcern = kCommandsSupportingWriteConcern.has(commandName);
        var shouldEmulateWriteConcern = kCommandsToEmulateWriteConcern.has(commandName);

        if (commandName === "aggregate") {
            if (OverrideHelpers.isAggregationWithOutStage(commandName, commandObjUnwrapped)) {
                // The $out stage can only be used with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            } else {
                // Only a $out aggregation does writes.
                shouldEmulateWriteConcern = false;
            }

            if (commandObjUnwrapped.explain) {
                // Attempting to specify a readConcern while explaining an aggregation would always
                // return an error prior to SERVER-30582 and it otherwise only compatible with
                // readConcern={level: "local"}.
                shouldForceReadConcern = false;
            }
        } else if (OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObjUnwrapped)) {
            // A writeConcern can only be used with non-inline output.
            shouldForceWriteConcern = false;
        } else if (commandObj[commandName] === "system.profile") {
            // Writes to the "system.profile" collection aren't guaranteed to be visible in the same
            // majority-committed snapshot as the command they originated from. We don't override
            // the readConcern for operations on the "system.profile" collection so that tests which
            // assert on its contents continue to succeed.
            shouldForceReadConcern = false;
        }

        const inWrappedForm = commandObj !== commandObjUnwrapped;

        if (shouldForceReadConcern) {
            // We create a copy of 'commandObj' to avoid mutating the parameter the caller
            // specified.
            commandObj = Object.assign({}, commandObj);
            if (inWrappedForm) {
                commandObjUnwrapped = Object.assign({}, commandObjUnwrapped);
                commandObj[Object.keys(commandObj)[0]] = commandObjUnwrapped;
            } else {
                commandObjUnwrapped = commandObj;
            }

            if (commandObjUnwrapped.hasOwnProperty("readConcern")) {
                var readConcern = commandObjUnwrapped.readConcern;

                if (typeof readConcern !== "object" || readConcern === null ||
                    (readConcern.hasOwnProperty("level") &&
                     bsonWoCompare({_: readConcern.level}, {_: kDefaultReadConcern.level}) !== 0)) {
                    throw new Error("Cowardly refusing to override read concern of command: " +
                                    tojson(commandObj));
                }

                // We create a copy of the readConcern object to avoid mutating the parameter the
                // caller specified.
                readConcern = Object.assign({}, readConcern, kDefaultReadConcern);
                commandObjUnwrapped.readConcern = readConcern;
            } else {
                commandObjUnwrapped.readConcern = kDefaultReadConcern;
            }
        }

        if (shouldForceWriteConcern) {
            // We create a copy of 'commandObj' to avoid mutating the parameter the caller
            // specified.
            commandObj = Object.assign({}, commandObj);
            if (inWrappedForm) {
                commandObjUnwrapped = Object.assign({}, commandObjUnwrapped);
                commandObj[Object.keys(commandObj)[0]] = commandObjUnwrapped;
            } else {
                commandObjUnwrapped = commandObj;
            }

            if (commandObjUnwrapped.hasOwnProperty("writeConcern")) {
                var writeConcern = commandObjUnwrapped.writeConcern;

                if (typeof writeConcern !== "object" || writeConcern === null ||
                    (writeConcern.hasOwnProperty("w") &&
                     bsonWoCompare({_: writeConcern.w}, {_: kDefaultWriteConcern.w}) !== 0)) {
                    throw new Error("Cowardly refusing to override write concern of command: " +
                                    tojson(commandObj));
                }

                // We create a copy of the writeConcern object to avoid mutating the parameter the
                // caller specified.
                writeConcern = Object.assign({}, writeConcern, kDefaultWriteConcern);
                commandObjUnwrapped.writeConcern = writeConcern;
            } else {
                commandObjUnwrapped.writeConcern = kDefaultWriteConcern;
            }
        }

        const serverResponse = func.apply(conn, makeFuncArgs(commandObj));

        if (shouldEmulateWriteConcern && serverResponse.ok === 1) {
            // We only wait for the write concern if the command succeeded to match what the
            // server's behavior would have been if the command supports the "writeConcern" option
            // itself.
            assert.commandWorked(
                conn.runCommand(dbName, Object.assign({getLastError: 1}, kDefaultWriteConcern), 0));
        }

        return serverResponse;
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/set_read_and_write_concerns.js");

    OverrideHelpers.overrideRunCommand(runCommandWithReadAndWriteConcerns);
})();
