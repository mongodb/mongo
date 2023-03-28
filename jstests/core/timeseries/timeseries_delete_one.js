/**
 * Tests running the deleteOne command on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesDeletesSupport,
 *   # TODO SERVER-73682: Enable this test.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = "timeseries_delete_one_";
let testCaseId = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a deleteOne() returns the expected set of documents.
 */
function testDeleteOne({initialDocList, filter, expectedResultDocs, nDeleted}) {
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.insert(initialDocList));

    const res = assert.commandWorked(coll.deleteOne(filter));
    assert.eq(nDeleted, res.deletedCount);

    const resultDocs = coll.find().toArray();
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

// Query on the 'f' field leads to zero measurement delete.
(function testZeroMeasurementDelete() {
    jsTestLog("Running testZeroMeasurementDelete()");
    testDeleteOne({
        initialDocList: [doc1_a_nofields, doc4_b_f103, doc6_c_f105],
        filter: {f: 17},
        expectedDocList: [doc1_a_nofields, doc4_b_f103, doc6_c_f105],
        nDeleted: 0,
    });
})();

// Query on the 'f' field leads to a partial bucket delete.
(function testPartialBucketDelete() {
    jsTestLog("Running testPartialBucketDelete()");
    testDeleteOne({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        filter: {f: 101},
        expectedDocList: [doc1_a_nofields, doc3_a_f102],
        nDeleted: 1,
    });
})();

// Query on the 'f' field leads to a full (single document) bucket delete.
(function testFullBucketDelete() {
    jsTestLog("Running testFullBucketDelete()");
    testDeleteOne({
        initialDocList: [doc2_a_f101],
        filter: {f: 101},
        expectedDocList: [],
        nDeleted: 1,
    });
})();

// Query on the 'tag' field matches all docs and deletes one.
(function testMatchFullBucketOnlyDeletesOne() {
    jsTestLog("Running testMatchFullBucketOnlyDeletesOne()");
    testDeleteOne({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        filter: {[metaFieldName]: "A"},
        // Don't validate exact results as we could delete any doc.
        nDeleted: 1,
    });
})();

// Query on the 'tag' and metric field.
(function testMetaAndMetricFilterOnlyDeletesOne() {
    jsTestLog("Running testMetaAndMetricFilterOnlyDeletesOne()");
    testDeleteOne({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        filter: {[metaFieldName]: "A", f: {$gt: 100}},
        // Don't validate exact results as we could delete any doc.
        nDeleted: 1,
    });
})();

// Query on the 'f' field matches docs in multiple buckets but only deletes from one.
(function testMatchMultiBucketOnlyDeletesOne() {
    jsTestLog("Running testMatchMultiBucketOnlyDeletesOne()");
    testDeleteOne({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        filter: {f: {$gt: 101}},
        // Don't validate exact results as we could delete one of a few docs.
        nDeleted: 1,
    });
})();

// Empty filter matches all docs but only deletes one.
(function testEmptyFilterOnlyDeletesOne() {
    jsTestLog("Running testEmptyFilterOnlyDeletesOne()");
    testDeleteOne({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        filter: {},
        // Don't validate exact results as we could delete any doc.
        nDeleted: 1,
    });
})();
})();
