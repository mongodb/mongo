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

const denylistedNamespaces = [
    /^admin\./,
    /^config\./,
    /\.system\./,
];

const timeValue = ISODate("2023-11-28T22:14:20.298Z");

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
        case "insert": {
            createCollectionImplicitly(
                conn.getDB(dbName), `${dbName}.${cmdObj[cmdName]}`, cmdObj[cmdName]);
            // Add the timestamp property to every document in the insert.
            if ("documents" in cmdObj) {
                cmdObj["documents"].forEach(doc => {
                    doc[timeFieldName] = timeValue;
                });
            }
            let insertResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if ("documents" in cmdObj) {
                cmdObj["documents"].forEach(doc => {
                    delete doc[timeFieldName];
                });
            }
            return insertResult;
        }
        case "create": {
            cmdObj["timeseries"] = {timeField: timeFieldName, metaField: metaFieldName};
            break;
        }
        case "find": {
            let findResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            cleanUpResultCursor(findResult, "firstBatch");
            return findResult;
        }
        case "findandmodify": {
            const upsert = cmdObj["upsert"] == true;
            if (upsert) {
                createCollectionImplicitly(
                    conn.getDB(dbName), `${dbName}.${cmdObj[cmdName]}`, cmdObj[cmdName]);
            }
            addTimeFieldForUpdate(cmdObj["update"], upsert);
            let findAndModifyResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            removeTimeFieldForUpdate(cmdObj["update"], upsert);
            if (findAndModifyResult["value"] != null) {
                delete findAndModifyResult["value"][timeFieldName];
            }
            return findAndModifyResult;
        }
        case "aggregate": {
            // Project the time field out of the aggregation result.
            const collAgg = typeof cmdObj["aggregate"] === "string";
            if (collAgg) {
                cmdObj["pipeline"].unshift({"$project": {[timeFieldName]: 0}});
            }
            let aggregateResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if (collAgg) {
                cmdObj["pipeline"].shift();
            }
            return aggregateResult;
        }
        case "getmore": {
            let getMoreResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            cleanUpResultCursor(getMoreResult, "nextBatch");
            return getMoreResult;
        }
        case "update": {
            if ("updates" in cmdObj) {
                cmdObj["updates"].forEach(upd => {
                    const upsert = upd["upsert"];
                    if (upsert) {
                        createCollectionImplicitly(
                            conn.getDB(dbName), `${dbName}.${cmdObj[cmdName]}`, cmdObj[cmdName]);
                    }
                    addTimeFieldForUpdate(upd["u"], upsert);
                });
                let updateResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
                cmdObj["updates"].forEach(upd => {
                    removeTimeFieldForUpdate(upd["u"], upd["upsert"]);
                });
                return updateResult;
            }
            break;
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

/**
 * Rewrites the assert equality check.
 */
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

/**
 * Creates the collection as time-series if it doesn't exist yet.
 */
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
 * Removes the time field from the cursor returned.
 */
function cleanUpResultCursor(result, batchName) {
    if (!("cursor" in result)) {
        return;
    }
    result["cursor"][batchName].forEach(doc => {
        delete doc[timeFieldName];
    })
}

/**
 * Sets the time field for update.
 */
function addTimeFieldForUpdate(upd, upsert) {
    if (upd == null) {
        return;
    }
    // Pipeline-style update.
    if (Array.isArray(upd)) {
        if (upsert) {
            upd.unshift({"$set": {[timeFieldName]: timeValue}});
        }
        return;
    }
    const opStyleUpd = Object.keys(upd).some(field => field.includes("$"));
    if (opStyleUpd) {
        // Op-style update.
        if (upsert) {
            if ("$set" in upd) {
                upd["$set"][timeFieldName] = timeValue;
            } else {
                upd["$set"] = {[timeFieldName]: timeValue};
            }
        }
    } else {
        // Replacement-style update.
        upd[timeFieldName] = timeValue;
    }
}

/**
 * Removes the time field for update.
 */
function removeTimeFieldForUpdate(upd, upsert) {
    if (upd == null) {
        return;
    }
    // Pipeline-style update.
    if (Array.isArray(upd)) {
        if (upsert) {
            upd.shift();
        }
        return;
    }
    const opStyleUpd = Object.keys(upd).some(field => field.includes("$"));
    if (opStyleUpd) {
        // Op-style update.
        if (upsert) {
            delete upd["$set"][timeFieldName];
            if (Object.keys(upd["$set"]).length === 0) {
                delete upd["$set"];
            }
        }
    } else {
        // Replacement-style update.
        delete upd[timeFieldName];
    }
}
