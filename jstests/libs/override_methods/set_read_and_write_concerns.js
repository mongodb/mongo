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

    const kDefaultReadConcern = {level: TestData.defaultReadConcernLevel};
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
        "parallelCollectionScan",
    ]);

    const kCommandsOnlySupportingReadConcernSnapshot = new Set([
        "delete",
        "findAndModify",
        "findandmodify",
        "insert",
        "update",
    ]);

    const kCommandsSupportingWriteConcern = new Set([
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
        "abortTransaction",
        "appendOplogNote",
        "applyOps",
        "aggregate",
        "captrunc",
        "cleanupOrphaned",
        "clone",
        "cloneCollection",
        "cloneCollectionAsCapped",
        "collMod",
        "commitTransaction",
        "convertToCapped",
        "copydb",
        "create",
        "createIndexes",
        "createRole",
        "createUser",
        "delete",
        "deleteIndexes",
        "doTxn",
        "drop",
        "dropAllRolesFromDatabase",
        "dropAllUsersFromDatabase",
        "dropDatabase",
        "dropIndexes",
        "dropRole",
        "dropUser",
        "emptycapped",
        "findAndModify",
        "findandmodify",
        "godinsert",
        "grantPrivilegesToRole",
        "grantRolesToRole",
        "grantRolesToUser",
        "insert",
        "mapReduce",
        "mapreduce",
        "mapreduce.shardedfinish",
        "moveChunk",
        "renameCollection",
        "revokePrivilegesFromRole",
        "revokeRolesFromRole",
        "revokeRolesFromUser",
        "setFeatureCompatibilityVersion",
        "update",
        "updateRole",
        "updateUser",
    ]);

    const kCommandsSupportingWriteConcernInTransaction =
        new Set(["doTxn", "abortTransaction", "commitTransaction"]);

    function runCommandWithReadAndWriteConcerns(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        let commandObjUnwrapped = commandObj;
        if (commandName === "query" || commandName === "$query") {
            commandObjUnwrapped = commandObj[commandName];
            commandName = Object.keys(commandObjUnwrapped)[0];
        }

        if (commandName === "eval" || commandName === "$eval") {
            throw new Error("Cowardly refusing to run test with overridden write concern when it" +
                            " uses a command that can only perform w=1 writes: " +
                            tojson(commandObj));
        }

        let shouldForceReadConcern = kCommandsSupportingReadConcern.has(commandName);
        let shouldForceWriteConcern = kCommandsSupportingWriteConcern.has(commandName);

        // All commands in a multi-document transaction have the autocommit property.
        if (commandObj.hasOwnProperty("autocommit")) {
            shouldForceReadConcern = false;
            if (!kCommandsSupportingWriteConcernInTransaction.has(commandName)) {
                shouldForceWriteConcern = false;
            }
        }
        if (commandName === "aggregate") {
            if (OverrideHelpers.isAggregationWithListLocalCursorsStage(commandName,
                                                                       commandObjUnwrapped)) {
                // The $listLocalCursors stage can only be used with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            }

            if (OverrideHelpers.isAggregationWithListLocalSessionsStage(commandName,
                                                                        commandObjUnwrapped)) {
                // The $listLocalSessions stage can only be used with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            }

            if (OverrideHelpers.isAggregationWithOutStage(commandName, commandObjUnwrapped)) {
                // The $out stage can only be used with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            } else {
                // A writeConcern can only be used with a $out stage.
                shouldForceWriteConcern = false;
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
        }

        if (kCommandsOnlySupportingReadConcernSnapshot.has(commandName) &&
            kDefaultReadConcern.level === "snapshot") {
            shouldForceReadConcern = true;
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
                let readConcern = commandObjUnwrapped.readConcern;

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
                let writeConcern = commandObjUnwrapped.writeConcern;

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

        return func.apply(conn, makeFuncArgs(commandObj));
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/set_read_and_write_concerns.js");

    OverrideHelpers.overrideRunCommand(runCommandWithReadAndWriteConcerns);
})();
