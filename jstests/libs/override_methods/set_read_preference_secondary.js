/**
 * Use prototype overrides to set read preference to "secondary" when running tests.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

const kReadPreferenceSecondary = {
    mode: "secondary"
};

const {defaultReadPreference: kReadPreferenceToUse = kReadPreferenceSecondary} = TestData;

const kCommandsSupportingReadPreference = new Set([
    "aggregate",
    "collStats",
    "count",
    "dbStats",
    "distinct",
    "find",
]);
const kDatabasesOnConfigServers = new Set(["config", "admin"]);

// This list of cursor-generating commands is incomplete. For example, both "listCollections" and
// "listIndexes" are missing from this list. If we ever add tests that attempt to run getMore or
// killCursors on cursors generated from those commands, then we should update the contents of
// this list and also handle any differences in the server's response format.
const kCursorGeneratingCommands = new Set(["aggregate", "find"]);

const CursorTracker = (function() {
    const kNoCursor = new NumberLong(0);

    const connectionsByCursorId = {};

    return {
        getConnectionUsedForCursor: function getConnectionUsedForCursor(cursorId) {
            return (cursorId instanceof NumberLong) ? connectionsByCursorId[cursorId] : undefined;
        },

        setConnectionUsedForCursor: function setConnectionUsedForCursor(cursorId, cursorConn) {
            if (cursorId instanceof NumberLong && !bsonBinaryEqual({_: cursorId}, {_: kNoCursor})) {
                connectionsByCursorId[cursorId] = cursorConn;
            }
        },
    };
})();

/**
 * Returns a random integer between the given range (inclusive).
 */
function getRandInteger(min, max) {
    return Math.floor(Math.random() * (max - min + 1)) + min;
}

/**
 * Returns a random element in the given array.
 */
function getRandomElement(arr) {
    return arr[getRandInteger(0, arr.length - 1)];
}

const kSecondariesToConnectDirectlyTo = [];
if (TestData.connectDirectlyToRandomSubsetOfSecondaries) {
    const hostColl = db.getSiblingDB("config").connectDirectlyToSecondaries.hosts;
    hostColl.find().forEach(doc => {
        if (doc.isSecondary && !doc.isExcluded) {
            kSecondariesToConnectDirectlyTo.push({host: doc.host, comment: doc.comment});
        }
    });

    if (kSecondariesToConnectDirectlyTo.length == 0) {
        // This is the first time this file is loaded. Choose the secondaries to connect
        // directly to.
        const helloRes = assert.commandWorked(db.adminCommand({hello: 1}));
        if (!helloRes.hasOwnProperty("setName")) {
            throw new Error(
                "Cannot connect directly to a secondary since this is not a replica set. " +
                "Unrecognized topology format:" + tojson(helloRes));
        }
        assert.gt(helloRes.passives.length, 0, {
            msg: "Cannot definitively determine which nodes are secondaries since all nodes " +
                "are electable",
            helloRes
        });
        assert.gt(helloRes.passives.length, 1, {
            msg: "Cannot connect to only a subset of secondaries since there is only one secondary",
            helloRes
        });

        jsTest.log("Choosing secondaries to reads directly from");
        assert.commandWorked(hostColl.insert({
            host: helloRes.primary,
            isPrimary: true,
        }));

        const secondaryToExclude =
            helloRes.passives[getRandInteger(0, helloRes.passives.length - 1)];
        helloRes.passives.forEach(host => {
            if (host == secondaryToExclude) {
                assert.commandWorked(hostColl.insert({host, isSecondary: true, isExcluded: true}));
            } else {
                const comment = extractUUIDFromObject(UUID());
                kSecondariesToConnectDirectlyTo.push({host, comment});
                assert.commandWorked(hostColl.insert({host, isSecondary: true, comment}));
            }
        });
    }
}
jsTest.log("Forcing reads to go directly to the following secondaries: " +
           tojsononeline(kSecondariesToConnectDirectlyTo));

function runCommandWithReadPreferenceSecondary(
    conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (commandObj[commandName] === "system.profile" || commandName === 'profile') {
        throw new Error(
            "Cowardly refusing to run test that interacts with the system profiler as the " +
            "'system.profile' collection is not replicated" + tojson(commandObj));
    }

    if (conn.isReplicaSetConnection() || TestData.connectDirectlyToRandomSubsetOfSecondaries) {
        // When a "getMore" or "killCursors" command is issued on a replica set connection, we
        // attempt to automatically route the command to the server the cursor(s) were
        // originally established on. This makes it possible to use the
        // set_read_preference_secondary.js override without needing to update calls of
        // DB#runCommand() to explicitly track the connection that was used. If the connection
        // is actually a direct connection to a mongod or mongos process, or if the cursor id
        // cannot be found in the CursorTracker, then we'll fall back to using DBClientRS's
        // server selection and send the operation to the current primary. It is possible that
        // the test is trying to exercise the behavior around when an unknown cursor id is sent
        // to the server.
        if (commandName === "getMore") {
            const cursorId = commandObj[commandName];
            const cursorConn = CursorTracker.getConnectionUsedForCursor(cursorId);
            if (cursorConn !== undefined) {
                return func.apply(cursorConn, makeFuncArgs(commandObj));
            }
        } else if (commandName === "killCursors") {
            const cursorIds = commandObj.cursors;
            if (Array.isArray(cursorIds)) {
                let cursorConn;

                for (let cursorId of cursorIds) {
                    const otherCursorConn = CursorTracker.getConnectionUsedForCursor(cursorId);
                    if (cursorConn === undefined) {
                        cursorConn = otherCursorConn;
                    } else if (otherCursorConn !== undefined) {
                        // We set 'cursorConn' back to undefined and break out of the loop so
                        // that we don't attempt to automatically route the "killCursors"
                        // command when there are cursors from different servers.
                        cursorConn = undefined;
                        break;
                    }
                }

                if (cursorConn !== undefined) {
                    return func.apply(cursorConn, makeFuncArgs(commandObj));
                }
            }
        }
    }

    let shouldForceReadPreference = kCommandsSupportingReadPreference.has(commandName);

    if ((commandName === "mapReduce" || commandName === "mapreduce") &&
        !OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObj)) {
        // A map-reduce operation with non-inline output must be sent to the primary.
        shouldForceReadPreference = false;
    } else if ((conn.isMongos() && kDatabasesOnConfigServers.has(dbName)) || conn._isConfigServer) {
        // Avoid overriding the read preference for config server since there may only be one
        // of them.
        shouldForceReadPreference = false;
    }

    if (commandName === "aggregate") {
        if (OverrideHelpers.isAggregationWithCurrentOpStage(commandName, commandObj)) {
            // Setting read preference secondary for an aggregation with $currentOp doesn't make
            // much sense, since there's no guarantee *which* secondary you get results from. We
            // will mirror the currentOp server command behavior here and maintain original read
            // preference.
            shouldForceReadPreference = false;
        }
    }

    if (TestData.doNotOverrideReadPreference) {
        // Use this TestData flag to allow certain runCommands to be exempted from
        // setting secondary read preference.
        shouldForceReadPreference = false;
    }

    if (shouldForceReadPreference) {
        if (commandObj.hasOwnProperty("$readPreference") &&
            !bsonBinaryEqual({_: commandObj.$readPreference}, {_: kReadPreferenceToUse})) {
            throw new Error("Cowardly refusing to override read preference to " +
                            tojson(kReadPreferenceToUse) + " for command: " + tojson(commandObj));
        } else if (!commandObj.hasOwnProperty("$readPreference")) {
            commandObj.$readPreference = kReadPreferenceToUse;
        }
        if (TestData.connectDirectlyToRandomSubsetOfSecondaries) {
            const randomSecondary = getRandomElement(kSecondariesToConnectDirectlyTo);

            const newConn =
                new Mongo("mongodb://" + randomSecondary.host + "/?directConnection=true");
            if (conn.isAutoEncryptionEnabled()) {
                const clientSideFLEOptions = conn.getAutoEncryptionOptions();
                assert(newConn.setAutoEncryption(clientSideFLEOptions));
                newConn.toggleAutoEncryption(true);
            }

            // To guarantee causal consistency, wait for the operationTime on the original
            // connection.
            const currentClusterTime = conn.getClusterTime();
            assert.soon(() => {
                const res = assert.commandWorked(newConn.adminCommand({"ping": 1}));
                return timestampCmp(res.operationTime, currentClusterTime.clusterTime) >= 0;
            });

            if (!commandObj.hasOwnProperty("comment")) {
                // If this command already has the "comment" field, do not overwrite it since that
                // could cause the test to fail.
                commandObj.comment = randomSecondary.comment;
            }
            conn = newConn;
        }
    }

    const serverResponse = func.apply(conn, makeFuncArgs(commandObj));

    if ((conn.isReplicaSetConnection() || TestData.connectDirectlyToRandomSubsetOfSecondaries) &&
        kCursorGeneratingCommands.has(commandName) && serverResponse.ok === 1 &&
        serverResponse.hasOwnProperty("cursor")) {
        // We associate the cursor id returned by the server with the connection that was used
        // to establish it so that we can attempt to automatically route subsequent "getMore"
        // and "killCursors" commands.
        CursorTracker.setConnectionUsedForCursor(serverResponse.cursor.id, serverResponse._mongo);
    }

    return serverResponse;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/set_read_preference_secondary.js");

OverrideHelpers.overrideRunCommand(runCommandWithReadPreferenceSecondary);
