/**
 * Tests running the delete command on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesDeletesSupport,
 *   # TODO SERVER-73319: Enable this test.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = "timeseries_delete_multi_";
let testCaseId = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a set of deletes returns the expected set of documents.
 */
function testDelete({initialDocList, deleteList, resultDocList, nDeleted}) {
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.insert(coll, initialDocList));

    const deleteCommand = {delete: coll.getName(), deletes: deleteList};
    const res = assert.commandWorked(testDB.runCommand(deleteCommand));

    assert.eq(nDeleted, res.n);
    const resDoc = coll.find().toArray();
    assert.eq(resultDocList.length, resDoc.length);

    resultDocList.forEach(resultDoc => {
        assert.docEq(resultDoc,
                     coll.findOne({_id: resultDoc._id}),
                     `Expected document (_id = ${resultDoc._id}) not found in result collection: ${
                         tojson(resDoc)}`);
    });
}

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

// Query on the _id field leads to zero measurement delete.
(function testZeroMeasurementDelete() {
    testDelete({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        deleteList: [{
            q: {_id: {$gt: 3}},
            limit: 0,
        }],
        resultDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        nDeleted: 0,
    });
})();

// Query on the _id field leads to a partial bucket delete.
(function testPartialBucketDelete1() {
    testDelete({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        deleteList: [{
            q: {_id: {$lt: 2}},
            limit: 0,
        }],
        resultDocList: [doc2_a_f101, doc3_a_f102],
        nDeleted: 1,
    });
})();

// Query on the _id field leads to a partial bucket delete.
(function testPartialBucketDelete2() {
    testDelete({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        deleteList: [{
            q: {_id: {$lt: 3}},
            limit: 0,
        }],
        resultDocList: [doc3_a_f102],
        nDeleted: 2,
    });
})();

// Query on the _id field leads to a full bucket delete.
(function testFullBucketDelete() {
    testDelete({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        deleteList: [{
            q: {[timeFieldName]: dateTime},
            limit: 0,
        }],
        resultDocList: [],
        nDeleted: 3,
    });
})();

// Query on the 'f' field leads to partial/full bucket deletes.
(function testMultiBucketDelete() {
    testDelete({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        deleteList: [{
            q: {f: {$lt: 106}},
            limit: 0,
        }],
        // Partial delete of bucket A and C, full delete of bucket B.
        resultDocList: [doc1_a_nofields, doc7_c_f106],
        nDeleted: 5,
    });
})();

// Query on meta results in scanning records that don't match the filter.
(function testMultiBucketDeleteComplexBucketFilter() {
    testDelete({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        deleteList: [
            {
                q: {f: {$in: [101, 102, 104, 105]}, [metaFieldName]: {$in: ["A", "C"]}},
                limit: 0,
            },
        ],
        resultDocList: [doc1_a_nofields, doc4_b_f103, doc5_b_f104, doc7_c_f106],
        nDeleted: 3,
    });
})();
})();
