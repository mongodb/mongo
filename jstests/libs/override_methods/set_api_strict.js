/**
 * Uses prototype overrides to set the api to strict for each command when running tests.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

const apiVersion = "1";

function runCommandWithApiStrict(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    // We create a copy of 'commandObj' to avoid mutating the parameter the caller specified.
    commandObj = Object.assign({}, commandObj);
    if (isOperationPartOfStableAPI(commandName, commandObj)) {
        commandObj.apiVersion = apiVersion;

        // Set the API to strict on the command object.
        commandObj.apiStrict = true;
    }

    return func.apply(conn, makeFuncArgs(commandObj));
}

function isOperationPartOfStableAPI(commandName, commandObj) {
    const stableCommands = new Set([
        "aggregate",
        "find",
        "createIndexes",
        "create",
        "update",
        "count",
        "abortTransaction",
        "authenticate",
        "collMod",
        "commitTransaction",
        "delete",
        "drop",
        "dropDatabase",
        "dropIndexes",
        "endSessions",
        "explain",
        "findAndModify",
        "getMore",
        "insert",
        "hello",
        "killCursors",
        "listCollections",
        "listDatabases",
        "listIndexes",
        "ping",
        "refreshSessions"
    ]);
    if (stableCommands.has(commandName)) {
        if (commandName == "aggregate" && commandObj.pipeline &&
            Array.isArray(commandObj.pipeline) && commandObj.pipeline.length > 0) {
            if (commandObj.pipeline[0].$indexStats ||
                (commandObj.pipeline[0].$collStats &&
                 (commandObj.pipeline[0].$collStats.latencyStats ||
                  commandObj.pipeline[0].$collStats.storageStats ||
                  commandObj.pipeline[0].$collStats.storageStats))) {
                return false;
            }
            for (let i = 0; i < commandObj.pipeline.length; i++) {
                if (commandObj.pipeline[i].$currentOp ||
                    commandObj.pipeline[i].$listLocalSessions ||
                    commandObj.pipeline[i].$listSessions ||
                    commandObj.pipeline[i].$planCacheStats || commandObj.pipeline[i].$search) {
                    return false;
                }
            }
        } else if (commandName == "find" &&
                   (commandObj.awaitData || commandObj.max || commandObj.min ||
                    commandObj.noCursorTimeout || commandObj.oplogReplay || commandObj.returnKey ||
                    commandObj.showRecordId || commandObj.tailable)) {
            return false;
        } else if (commandName == "create" &&
                   (commandObj.autoIndexId || commandObj.capped || commandObj.indexOptionDefaults ||
                    commandObj.max || commandObj.size || commandObj.storageEngine)) {
            return false;
        } else if (commandName == "createIndexes" && commandObj.indexes &&
                   Array.isArray(commandObj.indexes)) {
            for (let i = 0; i < commandObj.indexes.length; i++) {
                if (commandObj.indexes[i].key.text || commandObj.indexes[i].key.geoHaystack ||
                    commandObj.indexes[i].background || commandObj.indexes[i].bucketSize ||
                    commandObj.indexes[i].sparse || commandObj.indexes[i].storageEngine) {
                    return false;
                }
            }
        }
        return true;
    }
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/set_api_strict.js");

OverrideHelpers.overrideRunCommand(runCommandWithApiStrict);
})();
