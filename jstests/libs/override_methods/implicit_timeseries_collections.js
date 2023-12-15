/**
 * Loading this file will overwrite DB.prototype, DBCommandCursor.prototype, and the assert.eq
 * functions to create all collections as timeseries collections. As part of this override,
 * each insert/write will implicitly add a timeseries field to the document, which is
 * removed when calling any find methods.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
const originalGetCollection = DB.prototype.getCollection;
const originalDBCommandCursorNext = DBCommandCursor.prototype.next;
const originalAssertEq = assert.eq;

// The name of the implicitly added timestamp field.
const timeFieldName = "overrideTimeFieldName";
const metaFieldName = "metaFieldName"

// A set of dollar operators that need to be specially handled in the update command.
const dollarOperatorsSet = [
    "$set",
    "$setOnInsert",
    "$addFields",
];

const denylistedNamespaces = [
    /^admin\./,
    /^config\./,
    /\.system\./,
];

const timeValue = ISODate("2023-11-28T22:14:20.298Z");

/**
 * Creates the collection as time-series if it doesn't exist yet.
 */
DB.prototype.getCollection = function() {
    const collection = originalGetCollection.apply(this, arguments);
    createCollectionImplicitly(this, collection.getFullName(), collection.getName());
    return collection;
};

/**
 * Removes the timestamp field from the result of calling next on the cursor.
 */
DBCommandCursor.prototype.next = function() {
    let doc = originalDBCommandCursorNext.apply(this, arguments);
    delete doc[timeFieldName];
    return doc;
};

/**
 * Overrides DB command issue and handles commands that require the addition/removal of the
 * timestamp field where required.
 */
function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Command name is lowercase to account for variations in capitalization (i.e, findandmodify
    // should be handled the same way as findAndModify).
    switch (cmdName.toLowerCase()) {
        // Add the timestamp property to every document in the insert.
        case "insert": {
            createCollectionImplicitly(
                conn.getDB(dbName), `${dbName}.${cmdObj[cmdName]}`, cmdObj[cmdName]);
            for (let idx in cmdObj["documents"]) {
                cmdObj["documents"][idx][timeFieldName] = timeValue;
            }
            let insertResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            for (let idx in cmdObj["documents"]) {
                delete cmdObj["documents"][idx][timeFieldName];
            }
            return insertResult;
        }
        // Create a timeseries collection with the same name as the original createCollection
        // command.
        case "create": {
            cmdObj["timeseries"] = {timeField: timeFieldName, metaField: metaFieldName};
            break;
        }
        // Remove all instances of the timestamp field from the result of calling find.
        case "find": {
            let findResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if ("cursor" in findResult) {
                cleanUpResultCursor(findResult, "firstBatch");
            }
            return findResult;
        }
        // If the findandmodify involves an update, which could create a new document (with upsert =
        // true), then add the timestamp field to the update command. Also, remove the timestamp
        // field name from the result of the find command.
        case "findandmodify": {
            handleTSFieldForFindAndModify(cmdObj);
            let findAndModifyResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            handleTSFieldForFindAndModify(cmdObj, true /* remove = true */);
            if (findAndModifyResult["value"] != null) {
                delete findAndModifyResult["value"][timeFieldName];
            }
            return findAndModifyResult;
        }
        // Remove instances of the timestamp field name from the result of calling aggregate
        // functions.
        case "aggregate": {
            let aggregateResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if ("cursor" in aggregateResult) {
                cleanUpResultCursor(aggregateResult, "firstBatch");
            }
            return aggregateResult;
        }
        // Remove instances of the timestamp field name from the result of calling getmore.
        case "getmore": {
            let getMoreResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if ("cursor" in getMoreResult) {
                cleanUpResultCursor(getMoreResult, "nextBatch");
            }
            return getMoreResult;
        }
        case "update": {
            handleTSFieldForUpdates(cmdObj);
            let updateResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            handleTSFieldForUpdates(cmdObj, true /* remove = true */);
            return updateResult;
        }
        default: {
            break;
        }
    }
    // Call the original function, with a potentially modified command object.
    return clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// ----------------------- Rewriting Assert Functions ---------------------------------
assert.eq = function(a, b, message) {
    try {
        originalAssertEq(a, b, message);
    } catch (e) {
        // If the original assertion failed, and the failure came from comparing
        // two documents whose field order may have been changed by the insertion
        // of the time field, try comparing documents without taking the field order
        // into account.
        if ((a != null && typeof a == "object") && (b != null && typeof b == "object")) {
            assert.docEq(a, b, message);
        } else {
            throw e;
        }
    }
};

// ------------------------------ Helper Methods --------------------------------------

function createCollectionImplicitly(db, collFullName, collName) {
    for (const ns of denylistedNamespaces) {
        if (collFullName.match(ns)) {
            return;
        }
    }

    const collectionsList =
        new DBCommandCursor(
            db, db.runCommand({'listCollections': 1, nameOnly: true, filter: {name: collName}}))
            .toArray();

    if (collectionsList.length !== 0) {
        // Collection already exists.
        return;
    }

    db.runCommand(
        {create: collName, timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
}

/**
 * Helper method to remove instances of the timestamp field name from the cursor returned
 * in find (and findAndModify) calls.
 */
function cleanUpResultCursor(result, batchName) {
    let batch = result["cursor"][batchName];
    for (let i = 0; i < batch.length; i++) {
        delete batch[i][timeFieldName];
        for (let fieldName in batch[i]) {
            // Delete timeFieldName value from entries in nested array.
            if (Array.isArray(batch[i][fieldName])) {
                let arrCopy = [...batch[i][fieldName]];
                for (let j = 0; j < batch[i][fieldName].length; j++) {
                    let entry = batch[i][fieldName][j];
                    if (entry != null && typeof entry == "object" && timeFieldName in entry) {
                        arrCopy.splice(j, 1);
                    }
                }
                batch[i][fieldName] = arrCopy;
            }
            // Delete timeFieldName value from nested object.
            let entry = batch[i][fieldName];
            if (entry != null && typeof entry == "object") {
                delete entry[timeFieldName];
            }
        }
    }
}

/**
 * Helper method for either adding or removing the timeFieldName value from the update command
 * object, determined by whether remove is set to true or false.
 */
function handleTSFieldForUpdates(commandObj, remove = false) {
    for (const i in commandObj["updates"]) {
        // Aggregation pipeline case - "u" contains an array of operators.
        if (Array.isArray(commandObj["updates"][i]["u"])) {
            for (const j in commandObj["updates"][i]["u"]) {
                for (const field in commandObj["updates"][i]["u"][j]) {
                    if (dollarOperatorsSet.includes(field)) {
                        if (remove) {
                            delete commandObj["updates"][i]["u"][j][field][timeFieldName];
                        } else {
                            commandObj["updates"][i]["u"][j][field][timeFieldName] = timeValue;
                        }
                    }
                }
            }
        } else {
            const dollarOperators =
                Object.keys(commandObj["updates"][i]["u"]).filter(field => field.includes("$"));
            // Update operator case - each value in object should be an update operator.
            if (!dollarOperators.length == 0) {
                for (const field in commandObj["updates"][i]["u"]) {
                    // Certain operators potentially write a new document.
                    if (dollarOperatorsSet.includes(field)) {
                        if (remove) {
                            delete commandObj["updates"][i]["u"][field][timeFieldName];
                        } else {
                            commandObj["updates"][i]["u"][field][timeFieldName] = timeValue;
                        }
                    }
                }
            } else {  // Replacement document case - we can simply add/remove the timeFieldName
                      // value.
                if (remove) {
                    delete commandObj["updates"][i]["u"][timeFieldName];
                } else {
                    commandObj["updates"][i]["u"][timeFieldName] = timeValue;
                }
            }
        }
    }
}

/**
 * Helper method for either adding or removing the TS fields from the findAndModify command object,
 * determined by whether remove is set to true or false.
 */
function handleTSFieldForFindAndModify(commandObj, remove = false) {
    // Aggregation pipeline case - "u" contains an array of operators.
    if (Array.isArray(commandObj["update"])) {
        for (const i in commandObj["update"]) {
            for (const field in commandObj["update"][i]) {
                if (dollarOperatorsSet.includes(field)) {
                    if (remove) {
                        delete commandObj["update"][i][field][timeFieldName];
                    } else {
                        commandObj["update"][i][field][timeFieldName] = timeValue;
                    }
                }
            }
        }
    } else {
        if ("update" in commandObj) {
            const dollarOperators =
                Object.keys(commandObj["update"]).filter(field => field.includes("$"));
            // Update operator case - each value in object should be an update operator.
            if (!dollarOperators.length == 0) {
                for (const field in commandObj["update"]) {
                    // Certain operators potentially write a new document.
                    if (dollarOperatorsSet.includes(field)) {
                        if (remove) {
                            delete commandObj["update"][field][timeFieldName];
                        } else {
                            commandObj["update"][field][timeFieldName] = timeValue;
                        }
                    }
                }
            } else {  // Replacement document case - we can simply add/remove the timeFieldName
                // value
                if (remove) {
                    delete commandObj["update"][timeFieldName];
                } else {
                    commandObj["update"][timeFieldName] = timeValue;
                }
            }
        }
    }
}
