/**
 * Loading this file will overwrite DB.prototype, DBCommandCursor.prototype, and the assert.eq
 * functions to create all collections as timeseries collections. As part of this override,
 * each insert/write will implicitly add a timeseries field to the document, which is
 * removed when calling any find methods.
 */

import {sysCollNamePrefix} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {
    transformIndexHintsForTimeseriesCollection
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {
    checkSbeFullFeatureFlagEnabled,
} from "jstests/libs/query/sbe_util.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
const originalGetCollection = DB.prototype.getCollection;
const originalDBCommandCursorNext = DBCommandCursor.prototype.next;
const originalAssertEq = assert.eq;

// The name of the implicitly added timestamp field.
const timeFieldName = "overrideTimeFieldName";
const metaFieldName = "metaFieldName";

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
            // Only add the timeseries properties if we're not creating a view.
            if (!cmdObj.hasOwnProperty('viewOn')) {
                cmdObj["timeseries"] = {timeField: timeFieldName, metaField: metaFieldName};
            }
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
        case "explain": {
            // Project the time field out of the aggregation result for explain commands, if we
            // haven't already.
            const collAgg = typeof cmdObj["explain"]["aggregate"] === "string" &&
                bsonWoCompare(cmdObj["explain"]["pipeline"][0],
                              {"$project": {[timeFieldName]: 0}}) != 0;
            if (collAgg) {
                cmdObj["explain"]["pipeline"].unshift({"$project": {[timeFieldName]: 0}});
            }
            let aggregateResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if (collAgg) {
                cmdObj["explain"]["pipeline"].shift();
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
        case "setquerysettings": {
            return clientFunction.apply(
                conn, makeFuncArgs({
                    ...cmdObj,
                    setQuerySettings:
                        applyTimefieldProjectionToRepresentativeQuery(cmdObj.setQuerySettings)
                }));
        }
        case "removequerysettings": {
            return clientFunction.apply(
                conn, makeFuncArgs({
                    removeQuerySettings:
                        applyTimefieldProjectionToRepresentativeQuery(cmdObj.removeQuerySettings)
                }));
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
    });
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

const assertIndexScanStageInit = QuerySettingsIndexHintsTests.prototype.assertIndexScanStage;
QuerySettingsIndexHintsTests.prototype.assertIndexScanStage = function(cmd, expectedIndex, ns) {
    return assertIndexScanStageInit.call(this,
                                         cmd,
                                         transformIndexHintsForTimeseriesCollection(expectedIndex),
                                         {...ns, coll: sysCollNamePrefix + ns.coll});
};

QuerySettingsUtils.prototype.assertQueryFramework = function({query, settings, expectedEngine}) {
    // Find commands and aggregations which don't match the constraints from SERVER-88211 are
    // not eligible for SBE on timeseries collections.
    // TODO SERVER-95264 Extend the support for SBE Pushdowns for time series queries.
    if (query["find"] || (expectedEngine === "sbe" && !checkSbeFullFeatureFlagEnabled(db))) {
        jsTestLog(
            "Skipping assertions because sbe conditions for timeseries collections are not met.");
        return;
    }

    // Ensure that query settings cluster parameter is empty.
    this.assertQueryShapeConfiguration([]);

    // Apply the provided settings for the query.
    if (settings) {
        assert.commandWorked(this._db.adminCommand({setQuerySettings: query, settings: settings}));
        // Wait until the settings have taken effect.
        const expectedConfiguration = [this.makeQueryShapeConfiguration(settings, query)];
        this.assertQueryShapeConfiguration(expectedConfiguration);
    }

    const withoutDollarDB = query.aggregate ? {...this.withoutDollarDB(query), cursor: {}}
                                            : this.withoutDollarDB(query);
    const explain = assert.commandWorked(this._db.runCommand({explain: withoutDollarDB}));
    const engine = getEngine(explain);
    assert.eq(
        engine, expectedEngine, `Expected engine to be ${expectedEngine} but found ${engine}`);

    // Ensure that no $cursor stage exists, which means the whole query got pushed down to find,
    // if 'expectedEngine' is SBE.
    if (query.aggregate) {
        const cursorStages = getAggPlanStages(explain, "$cursor");

        if (expectedEngine === "sbe") {
            assert.eq(cursorStages.length, 0, cursorStages);
        } else {
            assert.gte(cursorStages.length, 0, cursorStages);
        }
    }

    // If a hinted index exists, assert it was used.
    if (query.hint) {
        const winningPlan = getWinningPlanFromExplain(explain);
        const ixscanStage = getPlanStages(winningPlan, "IXSCAN")[0];

        assert.eq(transformIndexHintsForTimeseriesCollection(query.hint),
                  ixscanStage.keyPattern,
                  winningPlan);
    }

    this.removeAllQuerySettings();
};

const assertQueryShapeConfigurationInit =
    QuerySettingsUtils.prototype.assertQueryShapeConfiguration;
QuerySettingsUtils.prototype.assertQueryShapeConfiguration = function(
    expectedQueryShapeConfigurations, shouldRunExplain = true) {
    const transformedQueryShapeConfigurations = expectedQueryShapeConfigurations.map(config => {
        if (!config["representativeQuery"]) {
            return config;
        }
        return {
            ...config,
            representativeQuery:
                applyTimefieldProjectionToRepresentativeQuery(config['representativeQuery'])
        };
    });
    return assertQueryShapeConfigurationInit.call(
        this, transformedQueryShapeConfigurations, shouldRunExplain);
};

const getQueryShapeHashFromQuerySettingsInit =
    QuerySettingsUtils.prototype.getQueryShapeHashFromQuerySettings;
QuerySettingsUtils.prototype.getQueryShapeHashFromQuerySettings = function(representativeQuery) {
    return getQueryShapeHashFromQuerySettingsInit.call(
        this, applyTimefieldProjectionToRepresentativeQuery(representativeQuery));
};

const getQuerySettingsInit = QuerySettingsUtils.prototype.getQuerySettings;
QuerySettingsUtils.prototype.getQuerySettings = function(
    {showDebugQueryShape = false, showQueryShapeHash = false, filter = undefined} = {}) {
    const settingsArr =
        getQuerySettingsInit.call(this, {showDebugQueryShape, showQueryShapeHash, filter});
    if (!showDebugQueryShape) {
        return settingsArr;
    }
    const newarr = settingsArr.map(settings => {
        const debugQueryShape = settings.debugQueryShape;
        if (!debugQueryShape) {
            return settings;
        }
        return {...settings, debugQueryShape: removeTimefieldProjectionFromQuery(debugQueryShape)};
    });
    return newarr;
};

/**
 * If the query is an aggregation, applies the projection which removes the timeField, if this stage
 * was not added already.
 */
function applyTimefieldProjectionToRepresentativeQuery(query) {
    if (typeof query["aggregate"] !== "string" ||
        bsonWoCompare(query["pipeline"][0], {"$project": {[timeFieldName]: 0}}) == 0) {
        return query;
    }
    let transformedQuery = TransactionsUtil.deepCopyObject({}, query);
    transformedQuery["pipeline"].unshift({"$project": {[timeFieldName]: 0}});
    return transformedQuery;
}

function removeTimefieldProjectionFromQuery(query) {
    if (!query || !query["pipeline"] || query["pipeline"].length === 0) {
        return query;
    }
    let transformedQuery = TransactionsUtil.deepCopyObject({}, query);
    const firstStage = transformedQuery["pipeline"][0];
    if (bsonWoCompare(firstStage, {"$project": {[timeFieldName]: false, "_id": true}}) == 0) {
        transformedQuery["pipeline"].shift();
    }
    return transformedQuery;
}
