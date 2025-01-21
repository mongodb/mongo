/**
 * Helpers for testing timeseries arbitrary writes.
 */

import {getExecutionStages, getPlanStage, getQueryPlanner} from "jstests/libs/analyze_plan.js";

export const timeFieldName = "time";
export const metaFieldName = "tag";
export const sysCollNamePrefix = "system.buckets.";

const tsIndexMinPrefix = "control.min.";
const tsIndexMaxPrefix = "control.max.";

export const closedBucketFilter = {
    "control.closed": {$not: {$eq: true}}
};

// The split point is between the 'A' and 'B' meta values which is _id: 4. [1, 3] goes to the
// primary shard and [4, 7] goes to the other shard.
export const splitMetaPointBetweenTwoShards = {
    meta: "B"
};

// This split point is the same as the 'splitMetaPointBetweenTwoShards'.
export const splitTimePointBetweenTwoShards = {
    [`control.min.${timeFieldName}`]: ISODate("2003-06-30")
};

export function generateTimeValue(index) {
    return ISODate(`${2000 + index}-01-01`);
}

// Defines sample data set for testing.
export const doc1_a_nofields = {
    _id: 1,
    [timeFieldName]: generateTimeValue(1),
    [metaFieldName]: "A",
};

export const doc2_a_f101 = {
    _id: 2,
    [timeFieldName]: generateTimeValue(2),
    [metaFieldName]: "A",
    f: 101
};

export const doc3_a_f102 = {
    _id: 3,
    [timeFieldName]: generateTimeValue(3),
    [metaFieldName]: "A",
    f: 102
};

export const doc4_b_f103 = {
    _id: 4,
    [timeFieldName]: generateTimeValue(4),
    [metaFieldName]: "B",
    f: 103
};

export const doc5_b_f104 = {
    _id: 5,
    [timeFieldName]: generateTimeValue(5),
    [metaFieldName]: "B",
    f: 104
};

export const doc6_c_f105 = {
    _id: 6,
    [timeFieldName]: generateTimeValue(6),
    [metaFieldName]: "C",
    f: 105
};

export const doc7_c_f106 = {
    _id: 7,
    [timeFieldName]: generateTimeValue(7),
    [metaFieldName]: "C",
    f: 106,
};

export let testDB = null;
export let st = null;
export let primaryShard = null;
export let otherShard = null;
export let mongos0DB = null;
export let mongos1DB = null;

/**
 * Composes and returns a bucket-level filter for timeseries arbitrary writes.
 *
 * The bucket-level filter is composed of the closed bucket filter and the given filter(s) which
 * are ANDed together. The closed bucket filter is always the first element of the AND array.
 */
export function makeBucketFilter(...args) {
    return {$and: [closedBucketFilter].concat(Array.from(args))};
}

export function getTestDB() {
    if (!testDB) {
        testDB = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDB.dropDatabase());
    }
    return testDB;
}

export function prepareCollection({dbToUse, collName, initialDocList, timeseriesOptions}) {
    if (!dbToUse) {
        dbToUse = getTestDB();
    }
    const coll = dbToUse.getCollection(collName);
    coll.drop();
    assert.commandWorked(dbToUse.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
            ...timeseriesOptions,
        }
    }));
    assert.commandWorked(coll.insert(initialDocList));

    return coll;
}

export function prepareShardedCollection(
    {dbToUse, collName, initialDocList, includeMeta = true, shardKey, splitPoint}) {
    if (!dbToUse) {
        assert.neq(
            null, testDB, "testDB must be initialized before calling prepareShardedCollection");
        dbToUse = testDB;
    }

    const coll = dbToUse.getCollection(collName);
    const sysCollName = sysCollNamePrefix + coll.getName();
    coll.drop();

    const tsOptions = includeMeta
        ? {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}
        : {timeseries: {timeField: timeFieldName}};
    assert.commandWorked(dbToUse.createCollection(coll.getName(), tsOptions));
    assert.commandWorked(coll.insert(initialDocList));

    if (!shardKey) {
        shardKey = includeMeta ? {[metaFieldName]: 1} : {[timeFieldName]: 1};
    }
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(
        dbToUse.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    if (!splitPoint) {
        splitPoint = includeMeta ? splitMetaPointBetweenTwoShards : splitTimePointBetweenTwoShards;
    }
    // [MinKey, splitPoint) and [splitPoint, MaxKey) are the two chunks after the split.
    assert.commandWorked(
        dbToUse.adminCommand({split: dbToUse[sysCollName].getFullName(), middle: splitPoint}));

    assert.commandWorked(dbToUse.adminCommand({
        moveChunk: dbToUse[sysCollName].getFullName(),
        find: splitPoint,
        to: otherShard.shardName,
        _waitForDelete: true
    }));

    return coll;
}

export function makeFindOneAndRemoveCommand(coll, filter, fields, sort, collation) {
    let findAndModifyCmd = {findAndModify: coll.getName(), query: filter, remove: true};
    if (fields) {
        findAndModifyCmd["fields"] = fields;
    }
    if (sort) {
        findAndModifyCmd["sort"] = sort;
    }
    if (collation) {
        findAndModifyCmd["collation"] = collation;
    }

    return findAndModifyCmd;
}

export function makeFindOneAndUpdateCommand(
    coll, filter, update, returnNew, upsert, fields, sort, collation) {
    assert(filter !== undefined && update !== undefined);
    let findAndModifyCmd = {findAndModify: coll.getName(), query: filter, update: update};
    if (returnNew !== undefined) {
        findAndModifyCmd["new"] = returnNew;
    }
    if (upsert !== undefined) {
        findAndModifyCmd["upsert"] = upsert;
    }
    if (fields !== undefined) {
        findAndModifyCmd["fields"] = fields;
    }
    if (sort !== undefined) {
        findAndModifyCmd["sort"] = sort;
    }
    if (collation !== undefined) {
        findAndModifyCmd["collation"] = collation;
    }

    return findAndModifyCmd;
}

/**
 * Returns the name of the caller of the function that called this function using the stack trace.
 *
 * This is useful for generating unique collection names. If the return function name is not unique
 * and the caller needs to generate a unique collection name, the caller can append a unique suffix.
 */
export function getCallerName(callDepth = 2) {
    return `${new Error().stack.split('\n')[callDepth].split('@')[0]}`;
}

export function verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted) {
    let resultDocs = coll.find().toArray();
    assert.eq(resultDocs.length, initialDocList.length - nDeleted, tojson(resultDocs));

    // Validate the collection's exact contents if we were given the expected results. We may skip
    // this step in some cases, if the delete doesn't pinpoint a specific document.
    if (expectedResultDocs) {
        assert.eq(expectedResultDocs.length, resultDocs.length, tojson(resultDocs));
        assert.sameMembers(expectedResultDocs, resultDocs, tojson(resultDocs));
    }
}

export function verifyExplain({
    explain,
    rootStageName,
    opType,
    bucketFilter,
    residualFilter,
    nBucketsUnpacked,
    nReturned,
    nMatched,
    nModified,
    nUpserted,
}) {
    jsTestLog(`Explain: ${tojson(explain)}`);
    assert(opType === "updateOne" || opType === "deleteOne" || opType === "updateMany" ||
           opType === "deleteMany");

    if (!rootStageName) {
        rootStageName = "TS_MODIFY";
    }
    assert("PROJECTION_DEFAULT" === rootStageName || "TS_MODIFY" === rootStageName,
           "Only PROJECTION_DEFAULT or TS_MODIFY is allowed");

    let foundStage = getPlanStage(explain.queryPlanner.winningPlan, rootStageName);
    assert.neq(null,
               foundStage,
               `The root ${rootStageName} stage not found in the plan: ${tojson(explain)}`);
    if (rootStageName === "PROJECTION_DEFAULT") {
        assert.eq("TS_MODIFY",
                  foundStage.inputStage.stage,
                  `TS_MODIFY is not a child of ${rootStageName} in the plan: ${tojson(explain)}`);
        foundStage = foundStage.inputStage;
    }

    assert.eq(opType, foundStage.opType, `TS_MODIFY opType is wrong: ${tojson(foundStage)}`);
    assert.eq(bucketFilter,
              foundStage.bucketFilter,
              `TS_MODIFY bucketFilter is wrong: ${tojson(foundStage)}`);
    assert.eq(residualFilter,
              foundStage.residualFilter,
              `TS_MODIFY residualFilter is wrong: ${tojson(foundStage)}`);

    const execStages = getExecutionStages(explain);
    assert.eq(rootStageName, execStages[0].stage, `The root stage is wrong: ${tojson(execStages)}`);
    let tsModifyStage = execStages[0];
    if (tsModifyStage.stage === "PROJECTION_DEFAULT") {
        tsModifyStage = tsModifyStage.inputStage;
    }
    assert.eq(
        "TS_MODIFY", tsModifyStage.stage, `Can't find TS_MODIFY stage: ${tojson(execStages)}`);

    if (nBucketsUnpacked !== undefined) {
        assert.eq(nBucketsUnpacked,
                  tsModifyStage.nBucketsUnpacked,
                  `Got wrong nBucketsUnpacked ${tojson(tsModifyStage)}`);
    }
    if (nReturned !== undefined) {
        assert.eq(
            nReturned, tsModifyStage.nReturned, `Got wrong nReturned ${tojson(tsModifyStage)}`);
    }
    if (nMatched !== undefined) {
        assert.eq(nMatched,
                  tsModifyStage.nMeasurementsMatched,
                  `Got wrong nMeasurementsMatched ${tojson(tsModifyStage)}`);
    }
    if (nModified !== undefined) {
        if (opType.startsWith("update")) {
            assert.eq(nModified,
                      tsModifyStage.nMeasurementsUpdated,
                      `Got wrong nMeasurementsModified ${tojson(tsModifyStage)}`);
        } else {
            assert.eq(nModified,
                      tsModifyStage.nMeasurementsDeleted,
                      `Got wrong nMeasurementsModified ${tojson(tsModifyStage)}`);
        }
    }
    if (nUpserted !== undefined) {
        assert.eq(nUpserted,
                  tsModifyStage.nMeasurementsUpserted,
                  `Got wrong nMeasurementsUpserted ${tojson(tsModifyStage)}`);
    }
}

/**
 * Verifies that a deleteOne returns the expected set of documents.
 *
 * - initialDocList: The initial documents in the collection.
 * - filter: The filter for the deleteOne command.
 * - expectedResultDocs: The expected documents in the collection after the delete.
 * - nDeleted: The expected number of documents deleted.
 */
export function testDeleteOne({initialDocList, filter, expectedResultDocs, nDeleted}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareCollection({collName: callerName, initialDocList: initialDocList});

    const res = assert.commandWorked(coll.deleteOne(filter));
    assert.eq(nDeleted, res.deletedCount);

    verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
}

export function getBucketCollection(coll) {
    return coll.getDB()[sysCollNamePrefix + coll.getName()];
}

/**
 * Ensure the updateOne command operates correctly by examining documents after the update.
 */
export function testUpdateOne({
    initialDocList,
    updateQuery,
    updateObj,
    c,
    collation,
    resultDocList,
    nMatched,
    nModified = nMatched,
    upsert = false,
    upsertedDoc,
    failCode,
    timeseriesOptions
}) {
    const collName = getCallerName();
    jsTestLog(`Running ${collName}(${tojson(arguments[0])})`);

    const testDB = getTestDB();
    const coll = testDB.getCollection(collName);
    prepareCollection({collName, initialDocList, timeseriesOptions});

    let upd = {q: updateQuery, u: updateObj, multi: false, upsert: upsert};
    if (c) {
        upd["c"] = c;
        upd["upsertSupplied"] = true;
    }
    if (collation) {
        upd["collation"] = collation;
    }
    const updateCommand = {
        update: coll.getName(),
        updates: [upd],
    };

    const res = failCode ? assert.commandFailedWithCode(testDB.runCommand(updateCommand), failCode)
                         : assert.commandWorked(testDB.runCommand(updateCommand));
    if (!failCode) {
        if (upsertedDoc) {
            assert.eq(1, res.n, tojson(res));
            assert.eq(0, res.nModified, tojson(res));
            assert(res.hasOwnProperty("upserted"), tojson(res));
            assert.eq(1, res.upserted.length);

            if (upsertedDoc.hasOwnProperty("_id")) {
                assert.eq(upsertedDoc._id, res.upserted[0]._id);
            } else {
                upsertedDoc["_id"] = res.upserted[0]._id;
            }
            resultDocList.push(upsertedDoc);
        } else {
            assert.eq(nMatched, res.n, tojson(res));
            assert.eq(nModified, res.nModified, tojson(res));
            assert(!res.hasOwnProperty("upserted"), tojson(res));
        }
    }

    if (resultDocList) {
        assert.sameMembers(resultDocList,
                           coll.find().toArray(),
                           "Collection contents did not match expected after update");
    }
}

export function testCollation(
    {testDB, coll, filter, update, queryCollation, nModified, expectedBucketQuery, expectedStage}) {
    let command;
    if (update) {
        command = {
            update: coll.getName(),
            updates: [{q: filter, u: update, multi: true, collation: queryCollation}],
        };
    } else {
        command = {
            delete: coll.getName(),
            deletes: [{q: filter, limit: 0, collation: queryCollation}],
        };
    }
    const explain = testDB.runCommand({explain: command, verbosity: "queryPlanner"});
    const parsedQuery = getQueryPlanner(explain).parsedQuery;

    assert.eq(expectedBucketQuery, parsedQuery, `Got wrong parsedQuery: ${tojson(explain)}`);
    assert.neq(null,
               getPlanStage(explain.queryPlanner.winningPlan, expectedStage),
               `${expectedStage} stage not found in the plan: ${tojson(explain)}`);

    const res = assert.commandWorked(testDB.runCommand(command));
    assert.eq(nModified, res.n);
}

/**
 * Verifies that a findAndModify remove returns the expected result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The filter for the findAndModify command.
 * - cmd.fields: The projection for the findAndModify command.
 * - cmd.sort: The sort option for the findAndModify command.
 * - cmd.collation: The collation option for the findAndModify command.
 * - res.errorCode: If errorCode is set, we expect the command to fail with that code and other
 *                  fields of 'res' are ignored.
 * - res.expectedResultDocs: The expected documents in the collection after the delete.
 * - res.nDeleted: The expected number of documents deleted.
 * - res.deletedDoc: The expected document returned by the findAndModify command.
 * - res.rootStage: The expected root stage of the explain plan.
 * - res.bucketFilter: The expected bucket filter of the TS_MODIFY stage.
 * - res.residualFilter: The expected residual filter of the TS_MODIFY stage.
 * - res.nBucketsUnpacked: The expected number of buckets unpacked by the TS_MODIFY stage.
 * - res.nReturned: The expected number of documents returned by the TS_MODIFY stage.
 * - timeseriesOptions: A document that will hold other time-series create collection options.
 */
export function testFindOneAndRemove({
    initialDocList,
    cmd: {filter, fields, sort, collation},
    res: {
        errorCode,
        expectedResultDocs,
        nDeleted,
        deletedDoc,
        rootStage,
        bucketFilter,
        residualFilter,
        nBucketsUnpacked,
        nReturned,
    },
    timeseriesOptions
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareCollection({
        collName: callerName,
        initialDocList: initialDocList,
        timeseriesOptions: timeseriesOptions
    });

    const findAndModifyCmd = makeFindOneAndRemoveCommand(coll, filter, fields, sort, collation);
    jsTestLog(`Running findAndModify remove: ${tojson(findAndModifyCmd)}`);

    const session = coll.getDB().getSession();
    const shouldRetryWrites = session.getOptions().shouldRetryWrites();
    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (coll.getDB().getSession().getOptions().shouldRetryWrites()) {
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), 7308305, `cmd = ${tojson(findAndModifyCmd)}`);
        return;
    }

    if (errorCode) {
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), errorCode, `cmd = ${tojson(findAndModifyCmd)}`);
        return;
    }

    if (bucketFilter !== undefined) {
        const explainRes = assert.commandWorked(
            coll.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"}));
        verifyExplain({
            explain: explainRes,
            rootStageName: rootStage,
            opType: "deleteOne",
            bucketFilter: bucketFilter,
            residualFilter: residualFilter,
            nBucketsUnpacked: nBucketsUnpacked,
            nReturned: nReturned,
        });
    }

    const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
    jsTestLog(`findAndModify remove result: ${tojson(res)}`);
    assert.eq(nDeleted, res.lastErrorObject.n, tojson(res));
    if (deletedDoc) {
        assert.docEq(deletedDoc, res.value, tojson(res));
    } else if (nDeleted === 1) {
        assert.neq(null, res.value, tojson(res));
    } else if (nDeleted === 0) {
        assert.eq(null, res.value, tojson(res));
    }

    verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
}

/**
 * Verifies that a findAndModify update returns the expected result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The 'query' spec for the findAndModify command.
 * - cmd.update: The 'update' spec for the findAndModify command.
 * - cmd.returnNew: The 'new' option for the findAndModify command.
 * - cmd.upsert: The 'upsert' option for the findAndModify command.
 * - cmd.fields: The projection for the findAndModify command.
 * - cmd.sort: The sort option for the findAndModify command.
 * - cmd.collation: The collation option for the findAndModify command.
 * - res.errorCode: If errorCode is set, we expect the command to fail with that code and other
 *                  fields of 'res' are ignored.
 * - res.resultDocList: The expected documents in the collection after the update.
 * - res.nModified: The expected number of documents deleted.
 * - res.returnDoc: The expected document returned by the findAndModify command.
 * - res.rootStage: The expected root stage of the explain plan.
 * - res.bucketFilter: The expected bucket filter of the TS_MODIFY stage.
 * - res.residualFilter: The expected residual filter of the TS_MODIFY stage.
 * - res.nBucketsUnpacked: The expected number of buckets unpacked by the TS_MODIFY stage.
 * - res.nMatched: The expected number of documents matched by the TS_MODIFY stage.
 * - res.nModified: The expected number of documents modified by the TS_MODIFY stage.
 * - res.nUpserted: The expected number of documents upserted by the TS_MODIFY stage.
 * - timeseriesOptions: A document that will hold other time-series create collection options.
 */
export function testFindOneAndUpdate({
    initialDocList,
    cmd: {filter, update, returnNew, upsert, fields, sort, collation},
    res: {
        errorCode,
        resultDocList,
        returnDoc,
        rootStage,
        bucketFilter,
        residualFilter,
        nBucketsUnpacked,
        nMatched,
        nModified,
        nUpserted,
    },
    timeseriesOptions
}) {
    const collName = getCallerName();
    jsTestLog(`Running ${collName}(${tojson(arguments[0])})`);

    const testDB = getTestDB();
    const coll = testDB.getCollection(collName);
    prepareCollection({collName, initialDocList, timeseriesOptions});

    const findAndModifyCmd = makeFindOneAndUpdateCommand(
        coll, filter, update, returnNew, upsert, fields, sort, collation);
    jsTestLog(`Running findAndModify update: ${tojson(findAndModifyCmd)}`);

    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (coll.getDB().getSession().getOptions().shouldRetryWrites()) {
        assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmd), 7314600);
        return;
    }

    if (errorCode) {
        assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmd), errorCode);
        return;
    }

    if (bucketFilter !== undefined) {
        const explainRes = assert.commandWorked(
            coll.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"}));
        verifyExplain({
            explain: explainRes,
            rootStageName: rootStage,
            opType: "updateOne",
            bucketFilter: bucketFilter,
            residualFilter: residualFilter,
            nBucketsUnpacked: nBucketsUnpacked,
            nReturned: returnDoc ? 1 : 0,
            nMatched: nMatched,
            nModified: nModified,
            nUpserted: nUpserted,
        });
    }

    const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
    jsTestLog(`findAndModify update result: ${tojson(res)}`);
    if (upsert) {
        assert(nUpserted !== undefined && (nUpserted === 0 || nUpserted === 1),
               "nUpserted must be 0 or 1");

        assert.eq(1, res.lastErrorObject.n, tojson(res));
        if (returnNew !== undefined) {
            assert(returnDoc, "returnDoc must be provided when upsert are true");
            assert.docEq(returnDoc, res.value, tojson(res));
        }

        if (nUpserted === 1) {
            assert(res.lastErrorObject.upserted, `Expected upserted ObjectId: ${tojson(res)}`);
            assert.eq(false, res.lastErrorObject.updatedExisting, tojson(res));
        } else {
            assert(!res.lastErrorObject.upserted, `Expected no upserted ObjectId: ${tojson(res)}`);
            assert.eq(true, res.lastErrorObject.updatedExisting, tojson(res));
        }
    } else {
        if (returnDoc !== undefined && returnDoc !== null) {
            assert.eq(1, res.lastErrorObject.n, tojson(res));
            assert.eq(true, res.lastErrorObject.updatedExisting, tojson(res));
            assert.docEq(returnDoc, res.value, tojson(res));
        } else {
            assert.eq(0, res.lastErrorObject.n, tojson(res));
            assert.eq(false, res.lastErrorObject.updatedExisting, tojson(res));
            assert.eq(null, res.value, tojson(res));
        }
    }

    if (resultDocList !== undefined) {
        assert.sameMembers(resultDocList,
                           coll.find().toArray(),
                           "Collection contents did not match expected after update");
    }
}

export function getRelevantProfilerEntries(db, coll, requestType) {
    const collName = coll.getName();
    const sysCollName = sysCollNamePrefix + coll.getName();
    const profilerFilter = {
        $or: [
            // Potential two-phase protocol cluster query.
            {
                "op": "command",
                "ns": `${db.getName()}.${sysCollName}`,
                "command.aggregate": `${sysCollName}`,
                "command.$_isClusterQueryWithoutShardKeyCmd": true,
                // Filters out events recorded because of StaleConfig error.
                "ok": {$ne: 0},
            },
            // Potential two-phase protocol write command and targeted write command.
            {
                "op": "command",
                "ns": `${db.getName()}.${collName}`,
                [`command.${requestType}`]: `${collName}`,
            }
        ]
    };
    return db.system.profile.find(profilerFilter).toArray();
}

export function verifyThatRequestIsRoutedToCorrectShard(
    coll, requestType, writeType, dataBearingShard) {
    assert(primaryShard && otherShard, "The sharded cluster must be initialized");
    assert(dataBearingShard === "primary" || dataBearingShard === "other" ||
               dataBearingShard === "none" || dataBearingShard === "any",
           "Invalid shard: " + dataBearingShard);
    assert(writeType === "twoPhaseProtocol" || writeType === "targeted",
           "Invalid write type: " + writeType);
    assert(requestType === "findAndModify" || requestType === "delete" || requestType === "update",
           "Invalid request type: " + requestType);

    const primaryDB = primaryShard.getDB(testDB.getName());
    const otherDB = otherShard.getDB(testDB.getName());

    const primaryEntries = getRelevantProfilerEntries(primaryDB, coll, requestType);
    const otherEntries = getRelevantProfilerEntries(otherDB, coll, requestType);

    /*
     * The profiler entries for the two-phase protocol are expected to be in the following order:
     * On the data bearing shard:
     * 1. Cluster query.
     * 2. Targeted request.
     *
     * On the non-data bearing shard:
     * 1. Cluster query.
     *
     * The profiler entries for the targeted write are expected to be in the following order:
     * On the data bearing shard:
     * 1. Targeted request.
     */

    if (dataBearingShard === "none") {
        // If dataBearingShard is "none", the writeType must be "twoPhaseProtocol". So, no shards
        // should get the targeted request after the cluster query for the case of "none".

        assert.eq("twoPhaseProtocol",
                  writeType,
                  "Expected data bearing shard to be 'none' only for 'twoPhaseProtocol' mode");

        assert.eq(1, primaryEntries.length, "Expected one profiler entry on primary shard");
        // The entry must be for the cluster query.
        assert(primaryEntries[0].command.hasOwnProperty("aggregate"),
               "Unexpected profile entries: " + tojson(primaryEntries));

        assert.eq(1, otherEntries.length, "Expected one profiler entry on other shard");
        // The entry must be for the cluster query.
        assert(otherEntries[0].command.hasOwnProperty("aggregate"),
               "Unexpected profile entries: " + tojson(otherEntries));
        return;
    }

    const [dataBearingShardEntries, nonDataBearingShardEntries] = (() => {
        if (dataBearingShard === "any") {
            assert.eq("twoPhaseProtocol",
                      writeType,
                      "Expected data bearing shard to be 'any' only for 'twoPhaseProtocol' mode");
            return primaryEntries.length === 2 ? [primaryEntries, otherEntries]
                                               : [otherEntries, primaryEntries];
        }

        return dataBearingShard === "primary" ? [primaryEntries, otherEntries]
                                              : [otherEntries, primaryEntries];
    })();

    if (writeType === "twoPhaseProtocol") {
        // At this point, we know that the data bearing shard is either primary or other. So, we
        // expect two profiler entries on the data bearing shard and one on the non-data bearing
        // shard.

        assert.eq(
            2,
            dataBearingShardEntries.length,
            `Expected two profiler entries for data bearing shard in 'twoPhaseProtocol' mode but
            got: ${tojson(dataBearingShardEntries)}`);
        // The first entry must be for the cluster query.
        assert(dataBearingShardEntries[0].command.hasOwnProperty("aggregate"),
               "Unexpected profile entries: " + tojson(dataBearingShardEntries));
        // The second entry must be the findAndModify command.
        assert(dataBearingShardEntries[1].command.hasOwnProperty(requestType),
               "Unexpected profile entries: " + tojson(dataBearingShardEntries));

        assert.eq(
            1,
            nonDataBearingShardEntries.length,
            `Expected one profiler entry for non data bearing shard in 'twoPhaseProtocol' mode but
            got: ${tojson(nonDataBearingShardEntries)}`);
        // The first entry must be for the cluster query.
        assert(nonDataBearingShardEntries[0].command.hasOwnProperty("aggregate"),
               "Unexpected profile entries: " + tojson(nonDataBearingShardEntries));
    } else {
        // This is the targeted write case. So, we expect one profiler entry on the data bearing
        // shard and none on the non-data bearing shard.

        assert.eq(1, dataBearingShardEntries.length, tojson(dataBearingShardEntries));
        // The first entry must be the findAndModify command.
        assert(dataBearingShardEntries[0].command.hasOwnProperty(requestType),
               "Unexpected profile entries: " + tojson(dataBearingShardEntries));

        assert.eq(0, nonDataBearingShardEntries.length, tojson(nonDataBearingShardEntries));
    }
}

export function restartProfiler() {
    assert(primaryShard && otherShard, "The sharded cluster must be initialized");

    const primaryDB = primaryShard.getDB(testDB.getName());
    const otherDB = otherShard.getDB(testDB.getName());

    primaryDB.setProfilingLevel(0);
    primaryDB.system.profile.drop();
    primaryDB.setProfilingLevel(2);
    otherDB.setProfilingLevel(0);
    otherDB.system.profile.drop();
    otherDB.setProfilingLevel(2);
}

/**
 * Verifies that a findAndModify remove on a sharded timeseries collection returns the expected
 * result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The filter for the findAndModify command.
 * - cmd.fields: The projection for the findAndModify command.
 * - cmd.sort: The sort option for the findAndModify command.
 * - cmd.collation: The collation option for the findAndModify command.
 * - res.errorCode: If errorCode is set, we expect the command to fail with that code and other
 *                  fields of 'res' are ignored.
 * - res.nDeleted: The expected number of documents deleted.
 * - res.deletedDoc: The expected document returned by the findAndModify command.
 * - res.writeType: "twoPhaseProtocol" or "targeted". On sharded time-series collection, we route
 *                  queries to shards if the queries contain the shardkey. "twoPhaseProtocol" means
 *                  that we cannot target a specific data-bearing shard from the query and should
 *                  the scatter-gather-like two-phase protocol. On the other hand, "targeted" means
 *                  we can from the query.
 * - res.dataBearingShard: "primary", "other", "none", or "any". For "none" and "any", only
 *                         the "twoPhaseProtocol" is allowed.
 * - res.rootStage: The expected root stage of the explain plan.
 * - res.bucketFilter: The expected bucket filter of the TS_MODIFY stage.
 * - res.residualFilter: The expected residual filter of the TS_MODIFY stage.
 * - res.nBucketsUnpacked: The expected number of buckets unpacked by the TS_MODIFY stage.
 * - res.nReturned: The expected number of documents returned by the TS_MODIFY stage.
 */
export function testFindOneAndRemoveOnShardedCollection({
    initialDocList,
    includeMeta = true,
    cmd: {filter, fields, sort, collation},
    res: {
        errorCode,
        nDeleted,
        deletedDoc,
        writeType,
        dataBearingShard,
        rootStage,
        bucketFilter,
        residualFilter,
        nBucketsUnpacked,
        nReturned,
    },
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareShardedCollection(
        {collName: callerName, initialDocList: initialDocList, includeMeta: includeMeta});

    const findAndModifyCmd = makeFindOneAndRemoveCommand(coll, filter, fields, sort, collation);
    jsTestLog(`Running findAndModify remove: ${tojson(findAndModifyCmd)}`);

    const session = coll.getDB().getSession();
    const shouldRetryWrites = session.getOptions().shouldRetryWrites();
    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (!shouldRetryWrites && !errorCode) {
        if (bucketFilter) {
            // Due to the limitation of two-phase write protocol, the TS_MODIFY stage's execution
            // stats can't really show the results close to real execution. We can just verify
            // plan part.
            assert(writeType !== "twoPhaseProtocol" || (!nBucketsUnpacked && !nReturned),
                   "Can't verify nBucketsUnpacked and nReturned for the two-phase protocol.");

            const explainRes = assert.commandWorked(
                coll.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"}));
            verifyExplain({
                explain: explainRes,
                rootStageName: rootStage,
                opType: "deleteOne",
                bucketFilter: bucketFilter,
                residualFilter: residualFilter,
                nBucketsUnpacked: nBucketsUnpacked,
                nReturned: nReturned,
            });
        }

        restartProfiler();
        const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
        jsTestLog(`findAndModify remove result: ${tojson(res)}`);
        assert.eq(nDeleted, res.lastErrorObject.n, tojson(res));
        let expectedResultDocs = initialDocList;
        if (deletedDoc) {
            // Note: To figure out the expected result documents, we need to know the _id of the
            // deleted document.
            assert(deletedDoc.hasOwnProperty("_id"),
                   `deletedDoc must have _id but got ${tojson(deletedDoc)}`);
            assert.docEq(deletedDoc, res.value, tojson(res));
            expectedResultDocs = initialDocList.filter(doc => doc._id !== deletedDoc._id);
        } else if (nDeleted === 1) {
            // Note: To figure out the expected result documents, we need to know the _id of the
            // deleted document. And so we don't allow 'fields' to be specified because it might
            // exclude _id field.
            assert(!fields, `Must specify deletedDoc when fields are specified: ${tojson(fields)}`);
            assert.neq(null, res.value, tojson(res));
            expectedResultDocs = initialDocList.filter(doc => doc._id !== res.value._id);
        } else if (nDeleted === 0) {
            assert.eq(null, res.value, tojson(res));
        }

        verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
        verifyThatRequestIsRoutedToCorrectShard(coll, "findAndModify", writeType, dataBearingShard);
    } else if (errorCode) {
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), errorCode, `cmd = ${tojson(findAndModifyCmd)}`);
    } else {
        // TODO SERVER-76583: Remove this test.
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), 7308305, `cmd = ${tojson(findAndModifyCmd)}`);
    }
}

/**
 * Verifies that a findAndModify update on a sharded timeseries collection returns the expected
 * result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The 'query' spec for the findAndModify command.
 * - cmd.update: The 'update' spec for the findAndModify command.
 * - cmd.returnNew: The 'new' option for the findAndModify command.
 * - cmd.upsert: The 'upsert' option for the findAndModify command.
 * - cmd.fields: The projection for the findAndModify command.
 * - cmd.sort: The sort option for the findAndModify command.
 * - cmd.collation: The collation option for the findAndModify command.
 * - res.errorCode: If errorCode is set, we expect the command to fail with that code and other
 *                  fields of 'res' are ignored.
 * - res.resultDocList: The expected documents in the collection after the update.
 * - res.returnDoc: The expected document returned by the findAndModify command.
 * - res.writeType: "twoPhaseProtocol" or "targeted". On sharded time-series collection, we route
 *                  queries to shards if the queries contain the shardkey. "twoPhaseProtocol" means
 *                  that we cannot target a specific data-bearing shard from the query and should
 *                  the scatter-gather-like two-phase protocol. On the other hand, "targeted" means
 *                  we can from the query.
 * - res.dataBearingShard: "primary", "other", "none", or "any". For "none" and "any", only
 *                         the "twoPhaseProtocol" is allowed.
 * - res.rootStage: The expected root stage of the explain plan.
 * - res.bucketFilter: The expected bucket filter of the TS_MODIFY stage.
 * - res.residualFilter: The expected residual filter of the TS_MODIFY stage.
 * - res.nBucketsUnpacked: The expected number of buckets unpacked by the TS_MODIFY stage.
 * - res.nMatched: The expected number of documents matched by the TS_MODIFY stage.
 * - res.nModified: The expected number of documents modified by the TS_MODIFY stage.
 * - res.nUpserted: The expected number of documents upserted by the TS_MODIFY stage.
 */
export function testFindOneAndUpdateOnShardedCollection({
    initialDocList,
    startTxn = false,
    includeMeta = true,
    cmd: {filter, update, returnNew, upsert, fields, sort, collation},
    res: {
        errorCode,
        resultDocList,
        returnDoc,
        writeType,
        dataBearingShard,
        rootStage,
        bucketFilter,
        residualFilter,
        nBucketsUnpacked,
        nMatched,
        nModified,
        nUpserted,
    },
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareShardedCollection(
        {collName: callerName, initialDocList: initialDocList, includeMeta: includeMeta});

    const findAndModifyCmd = makeFindOneAndUpdateCommand(
        coll, filter, update, returnNew, upsert, fields, sort, collation);
    jsTestLog(`Running findAndModify update: ${tojson(findAndModifyCmd)}`);

    if (errorCode) {
        assert.commandFailedWithCode(coll.runCommand(findAndModifyCmd), errorCode);
        assert.sameMembers(initialDocList,
                           coll.find().toArray(),
                           "Collection contents did not match expected after update failure.");
        return;
    }

    // Explain can't be run inside a transaction.
    if (!startTxn && bucketFilter) {
        // Due to the limitation of two-phase write protocol, the TS_MODIFY stage's execution
        // stats can't really show the results close to real execution. We can just verify
        // plan part.
        assert(writeType !== "twoPhaseProtocol" ||
                   (nBucketsUnpacked === undefined && nMatched === undefined &&
                    nModified === undefined),
               "Can't verify stats for the two-phase protocol.");

        const explainRes = assert.commandWorked(
            coll.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"}));
        verifyExplain({
            explain: explainRes,
            rootStageName: rootStage,
            opType: "updateOne",
            bucketFilter: bucketFilter,
            residualFilter: residualFilter,
            nBucketsUnpacked: nBucketsUnpacked,
            nReturned: returnDoc ? 1 : 0,
            nMatched: nMatched,
            nModified: nModified,
            nUpserted: nUpserted,
        });
    }

    restartProfiler();
    const res = (() => {
        if (!startTxn) {
            return assert.commandWorked(testDB.runCommand(findAndModifyCmd));
        }

        const session = coll.getDB().getMongo().startSession();
        const sessionDb = session.getDatabase(coll.getDB().getName());
        session.startTransaction();
        const res = assert.commandWorked(sessionDb.runCommand(findAndModifyCmd));
        session.commitTransaction();

        return res;
    })();
    jsTestLog(`findAndModify update result: ${tojson(res)}`);
    if (upsert) {
        assert(nUpserted !== undefined && (nUpserted === 0 || nUpserted === 1),
               "nUpserted must be 0 or 1");

        assert.eq(1, res.lastErrorObject.n, tojson(res));
        if (returnNew !== undefined) {
            assert(returnDoc, "returnDoc must be provided when upsert are true");
            assert.docEq(returnDoc, res.value, tojson(res));
        }

        if (nUpserted === 1) {
            assert(res.lastErrorObject.upserted, `Expected upserted ObjectId: ${tojson(res)}`);
            assert.eq(false, res.lastErrorObject.updatedExisting, tojson(res));
        } else {
            assert(!res.lastErrorObject.upserted, `Expected no upserted ObjectId: ${tojson(res)}`);
            assert.eq(true, res.lastErrorObject.updatedExisting, tojson(res));
        }
    } else {
        if (returnDoc !== undefined && returnDoc !== null) {
            assert.eq(1, res.lastErrorObject.n, tojson(res));
            assert.eq(true, res.lastErrorObject.updatedExisting, tojson(res));
            assert.docEq(returnDoc, res.value, tojson(res));
        } else {
            assert.eq(0, res.lastErrorObject.n, tojson(res));
            assert.eq(false, res.lastErrorObject.updatedExisting, tojson(res));
            assert.eq(null, res.value, tojson(res));
        }
    }

    if (resultDocList !== undefined) {
        assert.sameMembers(resultDocList,
                           coll.find().toArray(),
                           "Collection contents did not match expected after update");
    }

    verifyThatRequestIsRoutedToCorrectShard(coll, "findAndModify", writeType, dataBearingShard);
}

/**
 * Sets up a sharded cluster. 'nMongos' is the number of mongos in the cluster.
 */
export function setUpShardedCluster({nMongos} = {
    nMongos: 1
}) {
    assert.eq(null, st, "A sharded cluster must not be initialized yet");
    assert.eq(null, primaryShard, "The primary shard must not be initialized yet");
    assert.eq(null, otherShard, "The other shard must not be initialized yet");
    assert.eq(null, testDB, "testDB must be not initialized yet");
    assert.eq(null, mongos0DB, "mongos0DB must be not initialized yet");
    assert.eq(null, mongos1DB, "mongos1DB must be not initialized yet");
    assert(nMongos === 1 || nMongos === 2, "nMongos must be 1 or 2");

    st = new ShardingTest({mongos: nMongos, shards: 2, rs: {nodes: 2}});

    testDB = st.s.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    primaryShard = st.getPrimaryShard(testDB.getName());
    otherShard = st.getOther(primaryShard);
    mongos0DB = st.s0.getDB(testDB.getName());
    if (nMongos > 1) {
        mongos1DB = st.s1.getDB(testDB.getName());
    }
}

/**
 * Tears down the sharded cluster created by setUpShardedCluster().
 */
export function tearDownShardedCluster() {
    assert.neq(null, st, "A sharded cluster must be initialized");
    st.stop();
}

export function transformIndexHintsForTimeseriesCollection(indexHints) {
    let transformedIndexes = {};
    // Transform index to match timeseries control.min/max fields.
    for (const [key, value] of Object.entries(indexHints)) {
        transformedIndexes[tsIndexMinPrefix + key] = value;
        transformedIndexes[tsIndexMaxPrefix + key] = value;
    }
    return transformedIndexes;
}

export function transformIndexHintsFromTimeseriesToView(indexHints) {
    if (typeof indexHints !== "object") {
        return indexHints;
    }
    let transformedIndexes = {};
    // Transform index from control.min/max fields to normal keys.
    for (const [key, value] of Object.entries(indexHints)) {
        if (key.startsWith(tsIndexMinPrefix) || key.startsWith(tsIndexMaxPrefix)) {
            // Transform the index and add it if it was not already added (by scanning the
            // corresponding control.max.* or control.min.* index first).
            const transformedKey = key.substring(tsIndexMinPrefix.length);
            if (!transformedIndexes.hasOwnProperty(transformedKey)) {
                transformedIndexes[transformedKey] = value;
            }
        } else {
            // Also add non-bucket keys to index.
            transformedIndexes[key] = value;
        }
    }
    return transformedIndexes;
}
