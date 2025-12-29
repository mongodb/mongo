/**
 * Use prototype overrides to create collection with replicated record ids while running tests.
 *
 * The create command is overridden by blindly setting recordIdsReplicated to true.
 * The insert command is overridden by calling an additional recordIdsReplicated create command
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
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const collName = commandObj[commandName];
    const ns = dbName + "." + collName;
    const runCommand = (obj) => func.apply(conn, makeFuncArgs(obj));

    // Don't disturb errors that would be thrown for invalid collection names.
    if (!isValidCollectionName(collName)) {
        return runCommand(commandObj);
    }

    // Check drop before skipping tracked collections to avoid skipping drops.
    if (commandName === "drop") {
        collectionsKnownToExist.delete(ns);
        return runCommand(commandObj);
    }

    if (!commandsToOverride.has(commandName)) {
        return runCommand(commandObj);
    }

    if (collectionsKnownToExist.has(ns)) {
        return runCommand(commandObj);
    }

    switch (commandName) {
        case "create": {
            if (commandObj.hasOwnProperty("clusteredIndex") || commandObj.hasOwnProperty("timeseries")) {
                return runCommand(commandObj);
            }

            // Perform the modification.
            const createCmd = Object.assign({}, commandObj);
            if (!createCmd.hasOwnProperty("recordIdsReplicated")) {
                createCmd.recordIdsReplicated = true;
            }

            const res = runCommand(createCmd);
            if (hasError(res)) {
                jsTestLog(
                    "Error while creating collection via set_recordids_replicated.js override: " + tojsononeline(res),
                );
            }
            if (!hasError(res) || res.code === ErrorCodes.NamespaceExists) {
                collectionsKnownToExist.add(ns);
            }
            return res;
        }
        case "createIndexes": {
            const creatingClusteredIndex = commandObj["indexes"].some(
                (obj) => "clustered" in obj && obj.unique === true,
            );
            if (creatingClusteredIndex) {
                collectionsKnownToExist.add(ns);
                return runCommand(commandObj);
            }
            break;
        }
        case "insert":
        case "update": {
            if (commandObj.hasOwnProperty("documents")) {
                const idUndefinedInAnyDocument = commandObj["documents"].some(
                    (obj) => "_id" in obj && obj.id === undefined,
                );
                if (idUndefinedInAnyDocument) {
                    return runCommand(commandObj);
                }
            }
            break;
        }
        default:
            throw new Error(
                "Unexpected overridden command in set_recordids_replicated.js override: " + tojsononeline(commandObj),
            );
    }

    // If the collection already existed, don't clean up even if the inner command fails.
    const collExists = conn.getDB(dbName).getCollectionInfos({name: collName}).length > 0;
    if (collExists) {
        collectionsKnownToExist.add(ns);
        return runCommand(commandObj);
    }

    const createObj = {create: collName, recordIdsReplicated: true};
    for (const option of ["lsid", "$clusterTime", "writeConcern", "collectionUUID"]) {
        if (commandObj.hasOwnProperty(option)) {
            createObj[option] = commandObj[option];
        }
    }

    // Perform the modification by pre-creating the collection with recordIdsReplicated:true.
    const createRes = runCommand(createObj);
    let createdByOverride = false;
    if (hasError(createRes)) {
        jsTest.log.info(
            "Error while creating collection via set_recordids_replicated.js override: " + tojsononeline(createRes),
        );
    } else {
        createdByOverride = true;
    }
    if (!hasError(createRes) || createRes.code === ErrorCodes.NamespaceExists) {
        collectionsKnownToExist.add(ns);
    }

    // Now run the original command.
    const wrappedCmdRes = runCommand(commandObj);
    if (createdByOverride && hasError(wrappedCmdRes) && commandName === "createIndexes") {
        // Drop the collection we created to match implicit-collection semantics on createIndexes failure.
        jsTest.log.info(
            "Cleaning up collection created via set_recordids_replicated.js override after createIndexes error: " +
                tojsononeline(wrappedCmdRes),
        );
        const dropRes = runCommand({drop: collName});
        if (hasError(dropRes)) {
            jsTest.log.info(
                "Error while cleaning up collection via set_recordids_replicated.js override: " +
                    tojsononeline(dropRes),
            );
        }
        collectionsKnownToExist.delete(ns);
    }

    return wrappedCmdRes;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/collection_write_path/set_recordids_replicated.js",
);

OverrideHelpers.overrideRunCommand(runCommandWithRecordIdsReplicated);
