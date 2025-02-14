/*
 * This script provides a runCommand override which intercepts CRUD/aggregation commands, creates
 * histograms for all indexed fields and then dispatches to the original command. It is intended to
 * be used for passthrough testing of the cost-based ranker.
 */
import {
    getCollectionName,
    getInnerCommand,
    isSystemCollectionName,
    isTimeSeriesCollection,
} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// List of commands which this script will override to first construct histograms.
const queryCommands = [
    "aggregate",
    "count",
    "delete",
    "distinct",
    "explain",
    "find",
    "findAndModify",
    "update",
];

// Predicate function which returns true if a histogram over the given path can be constructed.
function isPathHistogrammable(path) {
    const components = path.split('.');
    return components.every(c => {
        return isNaN(Number(c)) &&  // Numeric path components are not supported.
            c !== "$**";            // Wildcard path not supported
    });
}

// Returns true if the given collection has a collation specified.
function collectionHasCollation(db, collectionName) {
    const collectionInfo = db.getCollectionInfos({name: collectionName});
    if (!collectionInfo || collectionInfo.length === 0) {
        return false;
    }
    return collectionInfo[0].options.hasOwnProperty("collation");
}

// Returns true if the collection can have 'analyze' run on it.
function isCollectionHistogrammable(db, coll) {
    // TODO SERVER-100679: Remove collation restriction
    return !coll.isCapped() && !isSystemCollectionName(coll.getName()) &&
        !isTimeSeriesCollection(db, coll.getName()) && !collectionHasCollation(db, coll.getName());
}

export function runCommandOverride(conn, dbName, _cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // If we are running a non-query command, dispatch the original command. This is necessary to do
    // here because the getCollectionName() function below will run a command itself, resulting in a
    // recursive call to this function.
    if (!queryCommands.includes(_cmdName)) {
        return clientFunction.apply(conn, makeFuncArgs(cmdObj));
    }

    const db = conn.getDB(dbName);
    const innerCmd = getInnerCommand(cmdObj);
    const collectionName = getCollectionName(db, innerCmd);
    const coll = db[collectionName];

    // Make histograms for all indexed fields.
    if (isCollectionHistogrammable(db, coll)) {
        // Drop statistics collection. This should not be necessary, but it is currently a
        // workaround for SERVER-100679.
        db.system.statistics[collectionName].drop();

        const indexes = coll.getIndexes();

        // Construct set of indexed paths
        let indexedPaths = new Set();
        indexes.forEach(index => Object.keys(index.key)
                                     .filter(isPathHistogrammable)
                                     .forEach(path => indexedPaths.add(path)));
        // Create histogram for each path
        indexedPaths.forEach(
            path => assert.commandWorked(db.runCommand({analyze: collectionName, key: path})));
    }

    // Dispatch to original command after creating histograms.
    return clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Install the runCommand hook
OverrideHelpers.overrideRunCommand(runCommandOverride);
