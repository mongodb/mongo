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
import {isValidCollectionName} from "jstests/libs/namespace_utils.js";

const commandsToOverride = new Set(["create", "insert", "update", "createIndexes"]);
// The set of collections already seen by this override and thus ignored.
const collectionsKnownToExist = new Set();

function hasError(res) {
    return res.ok !== 1 || res.writeErrors || (res.hasOwnProperty("nErrors") && res.nErrors != 0);
}

function runCommandWithRecordIdsReplicated(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    const collName = commandObj[commandName];
    const ns = dbName + "." + collName;
    if (commandName === "drop") {
        collectionsKnownToExist.delete(ns);
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (
        !commandsToOverride.has(commandName) ||
        collectionsKnownToExist.has(ns) ||
        typeof commandObj !== "object" ||
        commandObj === null
    ) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (!isValidCollectionName(collName)) {
        // Avoid issuing listCollections for invalid namespaces (e.g., embedded nulls) so we don't
        // mask the command's own InvalidNamespace error with our override logic.
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (commandName === "create") {
        if (commandObj.hasOwnProperty("clusteredIndex")) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }
        if (commandObj.hasOwnProperty("timeseries")) {
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
            collectionsKnownToExist.add(ns);
        }
        return res;
    } else {
        if (commandName === "createIndexes") {
            const clustered = "clustered";
            const clusteredTrue = commandObj["indexes"].some((obj) => clustered in obj && obj.unique === true);
            if (clusteredTrue) {
                collectionsKnownToExist.add(ns);
                return func.apply(conn, makeFuncArgs(commandObj));
            }
        }
        if ((commandName === "insert" || commandName === "update") && commandObj.hasOwnProperty("documents")) {
            const id = "_id";
            const idUndefined = commandObj["documents"].some((obj) => id in obj && obj.id === undefined);
            if (idUndefined) {
                return func.apply(conn, makeFuncArgs(commandObj));
            }
        }
        // If the collection already existed, don't clean up even if the inner command fails.
        const collExists = conn.getDB(dbName).getCollectionInfos({name: collName}).length > 0;
        if (collExists) {
            collectionsKnownToExist.add(ns);
            return func.apply(conn, makeFuncArgs(commandObj));
        }
        const createObj = {create: collName, recordIdsReplicated: true};
        ["lsid", "$clusterTime", "writeConcern", "collectionUUID"].forEach((option) => {
            if (commandObj.hasOwnProperty(option)) {
                createObj[option] = commandObj[option];
            }
        });

        const createRes = func.apply(conn, makeFuncArgs(createObj));
        let createdByOverride = false;
        if (hasError(createRes)) {
            jsTest.log.info(
                "Error while creating collection for set_recordids_replicated.js override: " + tojsononeline(createRes),
            );
        } else {
            createdByOverride = true;
        }
        if (!hasError(createRes) || createRes.code === ErrorCodes.NamespaceExists) {
            collectionsKnownToExist.add(ns);
        }

        const wrappedCmdRes = func.apply(conn, makeFuncArgs(commandObj));
        if (createdByOverride && hasError(wrappedCmdRes) && commandName === "createIndexes") {
            jsTest.log.info(
                "Cleaning up collection created for set_recordids_replicated.js override after createIndexes error: " +
                    tojsononeline(wrappedCmdRes),
            );
            const dropRes = func.apply(conn, makeFuncArgs({drop: collName}));
            if (hasError(dropRes)) {
                jsTest.log.info(
                    "Error while cleaning up collection for set_recordids_replicated.js override: " +
                        tojsononeline(dropRes),
                );
            }
            collectionsKnownToExist.delete(ns);
        }
        return wrappedCmdRes;
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/collection_write_path/set_recordids_replicated.js",
);

OverrideHelpers.overrideRunCommand(runCommandWithRecordIdsReplicated);
