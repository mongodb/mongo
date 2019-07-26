/**
 * Use prototype overrides to set read concern and write concern while running tests.
 *
 * A test can override the default read and write concern of commands by loading this library before
 * the test is run and setting the 'TestData.defaultReadConcernLevel' or
 * 'TestData.defaultWriteConcern' variables with the desired read/write concern level. For example,
 * setting:
 *
 * TestData.defaultReadConcernLevel = "majority"
 * TestData.writeConcernLevel = {w: "majority"}
 *
 * will run all commands with read/write concern "majority". It is also possible to only override
 * the write concern of commands, by setting 'TestData.defaultReadConcernLevel' = null. This will
 * not affect the default read concern of commands in any way.
 *
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");
load("jstests/libs/override_methods/read_and_write_concern_helpers.js");

if (typeof TestData === "undefined" || !TestData.hasOwnProperty("defaultReadConcernLevel")) {
    throw new Error("The readConcern level to use must be set as the 'defaultReadConcernLevel'" +
                    " property on the global TestData object");
}

// If the default read concern level is null, that indicates that no read concern overrides
// should be applied.
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
        if (OverrideHelpers.isAggregationWithListLocalSessionsStage(commandName,
                                                                    commandObjUnwrapped)) {
            // The $listLocalSessions stage can only be used with readConcern={level: "local"}.
            shouldForceReadConcern = false;
        }

        if (OverrideHelpers.isAggregationWithOutOrMergeStage(commandName, commandObjUnwrapped)) {
            // The $out stage can only be used with readConcern={level: "local"} or
            // readConcern={level: "majority"}
            if (TestData.defaultReadConcernLevel === "linearizable") {
                shouldForceReadConcern = false;
            }
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

    // Only override read concern if an override level was specified.
    if (shouldForceReadConcern && (kDefaultReadConcern.level !== null)) {
        // We create a copy of 'commandObj' to avoid mutating the parameter the caller
        // specified.
        commandObj = Object.assign({}, commandObj);
        if (inWrappedForm) {
            commandObjUnwrapped = Object.assign({}, commandObjUnwrapped);
            commandObj[Object.keys(commandObj)[0]] = commandObjUnwrapped;
        } else {
            commandObjUnwrapped = commandObj;
        }

        let readConcern;
        if (commandObjUnwrapped.hasOwnProperty("readConcern")) {
            readConcern = commandObjUnwrapped.readConcern;

            if (typeof readConcern !== "object" || readConcern === null ||
                (readConcern.hasOwnProperty("level") &&
                 bsonWoCompare({_: readConcern.level}, {_: kDefaultReadConcern.level}) !== 0)) {
                throw new Error("Cowardly refusing to override read concern of command: " +
                                tojson(commandObj));
            }
        }

        // We create a copy of the readConcern object to avoid mutating the parameter the
        // caller specified.
        readConcern = Object.assign({}, readConcern, kDefaultReadConcern);
        commandObjUnwrapped.readConcern = readConcern;
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

        let writeConcern;
        if (commandObjUnwrapped.hasOwnProperty("writeConcern")) {
            writeConcern = commandObjUnwrapped.writeConcern;

            if (typeof writeConcern !== "object" || writeConcern === null ||
                (writeConcern.hasOwnProperty("w") &&
                 bsonWoCompare({_: writeConcern.w}, {_: kDefaultWriteConcern.w}) !== 0)) {
                throw new Error("Cowardly refusing to override write concern of command: " +
                                tojson(commandObj));
            }
        }

        // We create a copy of the writeConcern object to avoid mutating the parameter the
        // caller specified.
        writeConcern = Object.assign({}, writeConcern, kDefaultWriteConcern);
        commandObjUnwrapped.writeConcern = writeConcern;
    }

    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/set_read_and_write_concerns.js");

OverrideHelpers.overrideRunCommand(runCommandWithReadAndWriteConcerns);
})();
