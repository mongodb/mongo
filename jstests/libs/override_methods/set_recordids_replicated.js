/**
 * Use prototype overrides to create collection with replicated record ids while running tests.
 *
 * The create command is overridden by blindly setting recordIdsReplicated to true.
 * The insert command is overriden by calling an additional recordIdsReplicated create command
 * before insert.
 *
 * A test can use the overrides by loading this library before the test is run.
 *
 * This is intended to be used by passthrough tests only.
 *
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const commandsToOverride = new Set(["create", "insert", "update", "createIndexes"]);
const createdCollections = new Set();

function hasError(res) {
    return res.ok !== 1 || res.writeErrors || (res.hasOwnProperty("nErrors") && res.nErrors != 0);
}

function runCommandWithRecordIdsReplicated(
    conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    const collName = commandObj[commandName];
    const ns = dbName + "." + collName;
    if (commandName === "drop") {
        createdCollections.delete(ns)
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (!commandsToOverride.has(commandName) || createdCollections.has(ns) ||
        typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (commandName === "create") {
        if (commandObj.hasOwnProperty("clusteredIndex")) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }
        commandObj = Object.assign({}, commandObj);
        if (!commandObj.hasOwnProperty("recordIdsReplicated")) {
            commandObj.recordIdsReplicated = true;
        }
        const res = func.apply(conn, makeFuncArgs(commandObj));
        if (hasError(res)) {
            jsTestLog("create error: " + tojsononeline(res));
        }
        if (!hasError(res) || res.code === ErrorCodes.NamespaceExists) {
            createdCollections.add(ns);
        }
        return res;
    } else {
        if (commandName === "createIndexes") {
            const clustered = "clustered";
            const clusteredTrue =
                commandObj["indexes"].some(obj => clustered in obj && obj.unique === true);
            if (clusteredTrue) {
                createdCollections.add(ns);
                return func.apply(conn, makeFuncArgs(commandObj));
            }
        }
        if ((commandName === "insert" || commandName === "update") &&
            commandObj.hasOwnProperty("documents")) {
            const id = "_id";
            const idUndefined =
                commandObj["documents"].some(obj => id in obj && obj.id === undefined);
            if (idUndefined) {
                return func.apply(conn, makeFuncArgs(commandObj));
            }
        }
        const createObj = {create: collName, recordIdsReplicated: true};
        ["lsid", "$clusterTime", "writeConcern", "collectionUUID"].forEach(option => {
            if (commandObj.hasOwnProperty(option)) {
                createObj[option] = commandObj[option];
            }
        });
        const res = func.apply(conn, makeFuncArgs(createObj));
        if (hasError(res)) {
            jsTestLog("create error: " + tojsononeline(res));
        }
        if (!hasError(res) || res.code === ErrorCodes.NamespaceExists) {
            createdCollections.add(ns);
        }
        return func.apply(conn, makeFuncArgs(commandObj));
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/set_recordids_replicated.js");

OverrideHelpers.overrideRunCommand(runCommandWithRecordIdsReplicated);
