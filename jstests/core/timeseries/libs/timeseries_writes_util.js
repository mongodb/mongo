/**
 * Helpers for testing timeseries arbitrary writes.
 */

load("jstests/libs/analyze_plan.js");     // For getPlanStage() and getExecutionStages().
load("jstests/libs/fixture_helpers.js");  // For 'isMongos'

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = "coll_";
const closedBucketFilter = {
    "control.closed": {$not: {$eq: true}}
};

let testCaseId = 0;
let testDB = null;

/**
 * Composes and returns a bucket-level filter for timeseries arbitrary writes.
 *
 * The bucket-level filter is composed of the closed bucket filter and the given filter(s) which
 * are ANDed together. The closed bucket filter is always the first element of the AND array.
 * Zero or more filters can be passed in as arguments.
 */
function makeBucketFilter(...args) {
    return {$and: [closedBucketFilter].concat(Array.from(args))};
}

function getTestDB() {
    if (!testDB) {
        testDB = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDB.dropDatabase());
    }
    return testDB;
}

function prepareCollection(initialDocList) {
    const testDB = getTestDB();
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(coll.insert(initialDocList));

    return coll;
}

function verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted) {
    let resultDocs = coll.find().toArray();
    assert.eq(resultDocs.length, initialDocList.length - nDeleted, tojson(resultDocs));

    // Validate the collection's exact contents if we were given the expected results. We may skip
    // this step in some cases, if the delete doesn't pinpoint a specific document.
    if (expectedResultDocs) {
        assert.eq(expectedResultDocs.length, resultDocs.length, resultDocs);
        expectedResultDocs.forEach(expectedDoc => {
            assert.docEq(
                expectedDoc,
                coll.findOne({_id: expectedDoc._id}),
                `Expected document (_id = ${expectedDoc._id}) not found in result collection: ${
                    tojson(resultDocs)}`);
        });
    }
}

function verifyExplain(
    {explain, rootStageName, bucketFilter, residualFilter, nBucketsUnpacked, nReturned}) {
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
 * Confirms that a deleteOne returns the expected set of documents.
 *
 * - initialDocList: The initial documents in the collection.
 * - filter: The filter for the deleteOne command.
 * - expectedResultDocs: The expected documents in the collection after the delete.
 * - nDeleted: The expected number of documents deleted.
 */
function testDeleteOne({initialDocList, filter, expectedResultDocs, nDeleted}) {
    const coll = prepareCollection(initialDocList);

    const res = assert.commandWorked(coll.deleteOne(filter));
    assert.eq(nDeleted, res.deletedCount);

    verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
}

/**
 * Confirms that a findAndModify with remove: true returns the expected result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The filter for the findAndModify command.
 * - cmd.fields: The projection for the findAndModify command.
 * - cmd.sort: The sort option for the findAndModify command.
 * - cmd.collation: The collation option for the findAndModify command.
 * - res.errorCode: If errorCode is set, we expect the command to fail with that code and other
 *                  fields of 'res' and 'explain' are ignored.
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
    const coll = prepareCollection(initialDocList);

    const session = coll.getDB().getSession();
    const shouldRetryWrites = session.getOptions().shouldRetryWrites();
    const findAndModifyCmd = {
        findAndModify: coll.getName(),
        query: filter,
        fields: fields,
        sort: sort,
        collation: collation,
        remove: true
    };
    // TODO SERVER-76583: Remove this check and always verify the result or verify the 'errorCode'.
    if (!shouldRetryWrites && !errorCode) {
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

        const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
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
        assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmd), errorCode);
    } else {
        // TODO SERVER-76583: Remove this test.
        assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmd), 7308305);
    }
}

// Defines sample data set for testing.
const doc1_a_nofields = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: "A",
};
const doc2_a_f101 = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: "A",
    f: 101
};
const doc3_a_f102 = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: "A",
    f: 102
};
const doc4_b_f103 = {
    _id: 4,
    [timeFieldName]: dateTime,
    [metaFieldName]: "B",
    f: 103
};
const doc5_b_f104 = {
    _id: 5,
    [timeFieldName]: dateTime,
    [metaFieldName]: "B",
    f: 104
};
const doc6_c_f105 = {
    _id: 6,
    [timeFieldName]: dateTime,
    [metaFieldName]: "C",
    f: 105
};
const doc7_c_f106 = {
    _id: 7,
    [timeFieldName]: dateTime,
    [metaFieldName]: "C",
    f: 106,
};

function getSampleDataForWrites() {
    return [
        doc1_a_nofields,
        doc2_a_f101,
        doc3_a_f102,
        doc4_b_f103,
        doc5_b_f104,
        doc6_c_f105,
        doc7_c_f106,
    ];
}
