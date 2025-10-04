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

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    kCommandsSupportingReadConcern,
    kCommandsSupportingSnapshot,
    kCommandsSupportingWriteConcern,
    kCommandsSupportingWriteConcernInTransaction,
    kWriteCommandsSupportingSnapshotInTransaction,
} from "jstests/libs/override_methods/read_and_write_concern_helpers.js";

if (typeof TestData === "undefined" || !TestData.hasOwnProperty("defaultReadConcernLevel")) {
    throw new Error(
        "The readConcern level to use must be set as the 'defaultReadConcernLevel'" +
            " property on the global TestData object",
    );
}

// If the default read concern level is null, that indicates that no read concern overrides
// should be applied.
const kDefaultReadConcern = {
    level: TestData.defaultReadConcernLevel,
};
const kDefaultWriteConcern = TestData.hasOwnProperty("defaultWriteConcern")
    ? TestData.defaultWriteConcern
    : {
          w: "majority",
          // Use a "signature" value that won't typically match a value assigned in normal use.
          // This way the wtimeout set by this override is distinguishable in the server logs.
          wtimeout: 5 * 60 * 1000 + 321, // 300321ms
      };

function runCommandWithReadAndWriteConcerns(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    let shouldForceReadConcern = kCommandsSupportingReadConcern.has(commandName);
    if (kDefaultReadConcern.level === "snapshot" && !kCommandsSupportingSnapshot.has(commandName)) {
        shouldForceReadConcern = false;
    } else if (
        TestData.disallowSnapshotDistinct &&
        kDefaultReadConcern.level === "snapshot" &&
        commandName === "distinct"
    ) {
        shouldForceReadConcern = false;
    }

    let shouldForceWriteConcern = kCommandsSupportingWriteConcern.has(commandName);

    // All commands in a multi-document transaction have the autocommit property.
    if (commandObj.hasOwnProperty("autocommit")) {
        shouldForceReadConcern = false;
        if (!kCommandsSupportingWriteConcernInTransaction.has(commandName)) {
            shouldForceWriteConcern = false;
        }
    }
    if (commandName === "aggregate") {
        if (OverrideHelpers.isAggregationWithListLocalSessionsStage(commandName, commandObj)) {
            // The $listLocalSessions stage can only be used with readConcern={level: "local"}.
            shouldForceReadConcern = false;
        }

        if (OverrideHelpers.isAggregationWithInternalListCollections(commandName, commandObj)) {
            // The $_internalListCollections stage can only be used with readConcern={level:
            // "local"}.
            shouldForceReadConcern = false;
        }

        // TODO (SERVER-98658) Reconsider if $listClusterCatalog still needs a local read concern
        if (OverrideHelpers.isAggregationWithListClusterCatalog(commandName, commandObj)) {
            // The $listClusterCatalog stage can only be used with readConcern={level:"local"}.
            shouldForceReadConcern = false;
        }

        if (OverrideHelpers.isAggregationWithChangeStreamStage(commandName, commandObj)) {
            // The $changeStream stage can only be used with readConcern={level: "majority"}.
            shouldForceReadConcern = false;
        }

        if (OverrideHelpers.isAggregationWithOutOrMergeStage(commandName, commandObj)) {
            // The $out stage can only be used with readConcern={level: "local"} or
            // readConcern={level: "majority"}
            if (TestData.defaultReadConcernLevel === "linearizable") {
                shouldForceReadConcern = false;
            }
        } else {
            // A writeConcern can only be used with a $out stage.
            shouldForceWriteConcern = false;
        }

        if (OverrideHelpers.isAggregationWithCurrentOpStage(commandName, commandObj)) {
            // The $currentOp stage can only be used with readConcern={level: "local"}.
            shouldForceReadConcern = false;
        }

        if (commandObj.explain) {
            // Attempting to specify a readConcern while explaining an aggregation would always
            // return an error prior to SERVER-30582 and it otherwise only compatible with
            // readConcern={level: "local"}.
            shouldForceReadConcern = false;
        }
    } else if (OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObj)) {
        // A writeConcern can only be used with non-inline output.
        shouldForceWriteConcern = false;
    } else if (commandName === "moveChunk") {
        // The moveChunk command automatically waits for majority write concern regardless of the
        // user-supplied write concern. Omitting the writeConcern option obviates the need to
        // specify the _secondaryThrottle=true option as well.
        shouldForceWriteConcern = false;
    }

    if (
        commandObj.hasOwnProperty("autocommit") &&
        kWriteCommandsSupportingSnapshotInTransaction.has(commandName) &&
        kDefaultReadConcern.level === "snapshot"
    ) {
        shouldForceReadConcern = true;
    }

    // Only override read concern if an override level was specified.
    if (shouldForceReadConcern && kDefaultReadConcern.level !== null) {
        // We create a copy of 'commandObj' to avoid mutating the parameter the caller specified.
        commandObj = Object.assign({}, commandObj);

        let readConcern;
        if (commandObj.hasOwnProperty("readConcern")) {
            readConcern = commandObj.readConcern;

            if (
                typeof readConcern !== "object" ||
                readConcern === null ||
                (readConcern.hasOwnProperty("level") &&
                    bsonWoCompare({_: readConcern.level}, {_: kDefaultReadConcern.level}) !== 0 &&
                    bsonWoCompare({_: readConcern.level}, {_: "local"}) !== 0 &&
                    bsonWoCompare({_: readConcern.level}, {_: "available"}) !== 0)
            ) {
                throw new Error("Cowardly refusing to override read concern of command: " + tojson(commandObj));
            }
        }

        // We create a copy of the readConcern object to avoid mutating the parameter the
        // caller specified.
        readConcern = Object.assign({}, readConcern, kDefaultReadConcern);
        commandObj.readConcern = readConcern;
    }

    if (shouldForceWriteConcern) {
        // We create a copy of 'commandObj' to avoid mutating the parameter the caller specified.
        commandObj = Object.assign({}, commandObj);

        let writeConcern;
        if (commandObj.hasOwnProperty("writeConcern")) {
            writeConcern = commandObj.writeConcern;

            if (
                typeof writeConcern !== "object" ||
                writeConcern === null ||
                (writeConcern.hasOwnProperty("w") &&
                    bsonWoCompare({_: writeConcern.w}, {_: kDefaultWriteConcern.w}) !== 0 &&
                    bsonWoCompare({_: writeConcern.w}, {_: 1}) !== 0)
            ) {
                throw new Error("Cowardly refusing to override write concern of command: " + tojson(commandObj));
            }
        }

        // We create a copy of the writeConcern object to avoid mutating the parameter the
        // caller specified.
        writeConcern = Object.assign({}, writeConcern, kDefaultWriteConcern);
        commandObj.writeConcern = writeConcern;
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/set_read_and_write_concerns.js");

OverrideHelpers.overrideRunCommand(runCommandWithReadAndWriteConcerns);
