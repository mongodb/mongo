/**
 * Helpers for testing timeseries arbitrary writes.
 */

load("jstests/libs/analyze_plan.js");  // For getPlanStage() and getExecutionStages().

const timeFieldName = "time";
const metaFieldName = "tag";
const sysCollNamePrefix = "system.buckets.";
const closedBucketFilter = {
    "control.closed": {$not: {$eq: true}}
};
// The split point is between the 'A' and 'B' meta values which is _id: 4. [1, 3] goes to the
// primary shard and [4, 7] goes to the other shard.
const splitMetaPointBetweenTwoShards = {
    meta: "B"
};
// This split point is the same as the 'splitMetaPointBetweenTwoShards'.
const splitTimePointBetweenTwoShards = {
    [`control.min.${timeFieldName}`]: ISODate("2003-06-30")
};

function generateTimeValue(index) {
    return ISODate(`${2000 + index}-01-01`);
}

// Defines sample data set for testing.
const doc1_a_nofields = {
    _id: 1,
    [timeFieldName]: generateTimeValue(1),
    [metaFieldName]: "A",
};
const doc2_a_f101 = {
    _id: 2,
    [timeFieldName]: generateTimeValue(2),
    [metaFieldName]: "A",
    f: 101
};
const doc3_a_f102 = {
    _id: 3,
    [timeFieldName]: generateTimeValue(3),
    [metaFieldName]: "A",
    f: 102
};
const doc4_b_f103 = {
    _id: 4,
    [timeFieldName]: generateTimeValue(4),
    [metaFieldName]: "B",
    f: 103
};
const doc5_b_f104 = {
    _id: 5,
    [timeFieldName]: generateTimeValue(5),
    [metaFieldName]: "B",
    f: 104
};
const doc6_c_f105 = {
    _id: 6,
    [timeFieldName]: generateTimeValue(6),
    [metaFieldName]: "C",
    f: 105
};
const doc7_c_f106 = {
    _id: 7,
    [timeFieldName]: generateTimeValue(7),
    [metaFieldName]: "C",
    f: 106,
};

let testDB = null;
let st = null;
let primaryShard = null;
let otherShard = null;

/**
 * Composes and returns a bucket-level filter for timeseries arbitrary writes.
 *
 * The bucket-level filter is composed of the closed bucket filter and the given filter(s) which
 * are ANDed together. The closed bucket filter is always the first element of the AND array.
 * Zero or more filters can be passed in as arguments.
 */
function makeBucketFilter(...args) {
    if (!args.length) {
        return closedBucketFilter;
    }

    return {$and: [closedBucketFilter].concat(Array.from(args))};
}

function getTestDB() {
    if (!testDB) {
        testDB = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDB.dropDatabase());
    }
    return testDB;
}

function prepareCollection({collName, initialDocList}) {
    const testDB = getTestDB();
    const coll = testDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(coll.insert(initialDocList));

    return coll;
}

function prepareShardedCollection({collName, initialDocList, includeMeta}) {
    assert.neq(null, testDB, "testDB must be initialized before calling prepareShardedCollection");

    const coll = testDB.getCollection(collName);
    const sysCollName = sysCollNamePrefix + coll.getName();
    coll.drop();

    const tsOptions = includeMeta
        ? {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}
        : {timeseries: {timeField: timeFieldName}};
    assert.commandWorked(testDB.createCollection(coll.getName(), tsOptions));
    assert.commandWorked(coll.insert(initialDocList));

    const shardKey = includeMeta ? {[metaFieldName]: 1} : {[timeFieldName]: 1};
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    const splitPoint =
        includeMeta ? splitMetaPointBetweenTwoShards : splitTimePointBetweenTwoShards;
    // [MinKey, splitPoint) and [splitPoint, MaxKey) are the two chunks after the split.
    assert.commandWorked(
        testDB.adminCommand({split: testDB[sysCollName].getFullName(), middle: splitPoint}));

    assert.commandWorked(testDB.adminCommand({
        moveChunk: testDB[sysCollName].getFullName(),
        find: splitPoint,
        to: otherShard.shardName,
        _waitForDelete: true
    }));

    return coll;
}

function makeFindAndModifyCommand(coll, filter, fields, sort, collation) {
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

/**
 * Returns the name of the caller of the function that called this function using the stack trace.
 *
 * This is useful for generating unique collection names. If the return function name is not unique
 * and the caller needs to generate a unique collection name, the caller can append a unique suffix.
 */
function getCallerName() {
    return `${new Error().stack.split('\n')[2].split('@')[0]}`;
}

function verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted) {
    let resultDocs = coll.find().toArray();
    assert.eq(resultDocs.length, initialDocList.length - nDeleted, tojson(resultDocs));

    // Validate the collection's exact contents if we were given the expected results. We may skip
    // this step in some cases, if the delete doesn't pinpoint a specific document.
    if (expectedResultDocs) {
        assert.eq(expectedResultDocs.length, resultDocs.length, tojson(resultDocs));
        assert.sameMembers(expectedResultDocs, resultDocs, tojson(resultDocs));
    }
}

function verifyExplain(
    {explain, rootStageName, bucketFilter, residualFilter, nBucketsUnpacked, nReturned}) {
    jsTestLog(`Explain: ${tojson(explain)}`);
    if (!rootStageName) {
        rootStageName = "TS_MODIFY";
    } else {
        assert.eq("PROJECTION_DEFAULT", rootStageName, "Only PROJECTION_DEFAULT is allowed");
    }

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

    assert.eq("deleteOne", foundStage.opType, `TS_MODIFY opType is wrong: ${tojson(foundStage)}`);
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
    assert.eq(nBucketsUnpacked,
              tsModifyStage.nBucketsUnpacked,
              `Got wrong nBucketsUnpacked ${tojson(tsModifyStage)}`);
    assert.eq(nReturned, tsModifyStage.nReturned, `Got wrong nReturned ${tojson(tsModifyStage)}`);
}

/**
 * Verifies that a deleteOne returns the expected set of documents.
 *
 * - initialDocList: The initial documents in the collection.
 * - filter: The filter for the deleteOne command.
 * - expectedResultDocs: The expected documents in the collection after the delete.
 * - nDeleted: The expected number of documents deleted.
 */
function testDeleteOne({initialDocList, filter, expectedResultDocs, nDeleted}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareCollection({collName: callerName, initialDocList: initialDocList});

    const res = assert.commandWorked(coll.deleteOne(filter));
    assert.eq(nDeleted, res.deletedCount);

    verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
}

function getBucketCollection(coll) {
    return coll.getDB()[sysCollNamePrefix + coll.getName()];
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
 */
function testFindOneAndRemove({
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
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareCollection({collName: callerName, initialDocList: initialDocList});

    const findAndModifyCmd = makeFindAndModifyCommand(coll, filter, fields, sort, collation);
    jsTestLog(`Running findAndModify remove: ${tojson(findAndModifyCmd)}`);

    const session = coll.getDB().getSession();
    const shouldRetryWrites = session.getOptions().shouldRetryWrites();
    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (!shouldRetryWrites && !errorCode) {
        const bucketColl = getBucketCollection(coll);
        // TODO SERVER-76906 Enable explain for findAndModify on a sharded timeseries collection.
        if (!FixtureHelpers.isSharded(bucketColl)) {
            const explainRes = assert.commandWorked(
                coll.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"}));
            if (bucketFilter) {
                verifyExplain({
                    explain: explainRes,
                    rootStageName: rootStage,
                    bucketFilter: bucketFilter,
                    residualFilter: residualFilter,
                    nBucketsUnpacked: nBucketsUnpacked,
                    nReturned: nReturned,
                });
            }
        } else {
            jsTestLog("Skipping explain for sharded timeseries collections");

            const collMetadata =
                db.getSiblingDB("config").collections.findOne({_id: bucketColl.getFullName()});
            jsTestLog(`Collection metadata -\n${tojson(collMetadata)}`);
            if (collMetadata.timestamp) {
                jsTestLog(`Shard info -\n${
                    tojson(db.getSiblingDB("config")
                               .chunks.find({uuid: collMetadata.uuid})
                               .toArray())}`);
            } else {
                jsTestLog(`Shard info -\n${
                    tojson(db.getSiblingDB("config")
                               .chunks.find({uuid: collMetadata.uuid})
                               .toArray())}`);
            }
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
    } else if (errorCode) {
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), errorCode, `cmd = ${tojson(findAndModifyCmd)}`);
    } else {
        // TODO SERVER-76583: Remove this test.
        assert.commandFailedWithCode(
            testDB.runCommand(findAndModifyCmd), 7308305, `cmd = ${tojson(findAndModifyCmd)}`);
    }
}

function getRelevantProfilerEntries(db, coll, requestType) {
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
            // Potential two-phase protocol write command.
            {
                "op": "command",
                "ns": `${db.getName()}.${sysCollName}`,
                [`command.${requestType}`]: `${sysCollName}`,
            },
            // Targeted write command.
            {
                "op": "command",
                "ns": `${db.getName()}.${sysCollName}`,
                [`command.${requestType}`]: `${coll.getName()}`,
            }
        ]
    };
    return db.system.profile.find(profilerFilter).toArray();
}

function verifyThatRequestIsRoutedToCorrectShard(coll, requestType, writeType, dataBearingShard) {
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

function restartProfiler() {
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
 */
function testFindOneAndRemoveOnShardedCollection({
    initialDocList,
    includeMeta = true,
    cmd: {filter, fields, sort, collation},
    res: {
        errorCode,
        nDeleted,
        deletedDoc,
        writeType,
        dataBearingShard,
    },
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareShardedCollection(
        {collName: callerName, initialDocList: initialDocList, includeMeta: includeMeta});

    const findAndModifyCmd = makeFindAndModifyCommand(coll, filter, fields, sort, collation);
    jsTestLog(`Running findAndModify remove: ${tojson(findAndModifyCmd)}`);

    const session = coll.getDB().getSession();
    const shouldRetryWrites = session.getOptions().shouldRetryWrites();
    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (!shouldRetryWrites && !errorCode) {
        // TODO SERVER-76906 Verify explain for findAndModify on sharded timeseries collections.
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
 * Sets up a sharded cluster.
 */
function setUpShardedCluster() {
    assert.eq(null, st, "A sharded cluster must not be initialized yet");
    assert.eq(null, primaryShard, "The primary shard must not be initialized yet");
    assert.eq(null, otherShard, "The other shard must not be initialized yet");
    assert.eq(null, testDB, "testDB must be not initialized yet");

    st = new ShardingTest({shards: 2, rs: {nodes: 2}});
    testDB = st.s.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    primaryShard = st.getPrimaryShard(testDB.getName());
    otherShard = st.getOther(primaryShard);
}

/**
 * Tears down the sharded cluster created by setUpShardedCluster().
 */
function tearDownShardedCluster() {
    assert.neq(null, st, "A sharded cluster must be initialized");
    st.stop();
}
