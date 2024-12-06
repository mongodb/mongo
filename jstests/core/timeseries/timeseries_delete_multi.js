/**
 * Tests running the delete command on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   requires_fcv_70,
 * ]
 */

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const bucketParam = 3600;
const collNamePrefix = "timeseries_delete_multi_";
let testCaseId = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a set of deletes returns the expected set of documents.
 */
function testDelete({initialDocList, deleteList, resultDocList, nDeleted, timeseriesOptions}) {
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(
        coll.getName(),
        {timeseries: {timeField: timeFieldName, metaField: metaFieldName, ...timeseriesOptions}}));

    assert.commandWorked(coll.insert(initialDocList));

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
const times = [
    new Date(dateTime.getTime() - (bucketParam * 1000) / 2),
    dateTime,
    new Date(dateTime.getTime() + (bucketParam * 1000) / 2),
    new Date(dateTime.getTime() + (bucketParam * 1000)),
];

const doc_id_8_time_f101 = {
    _id: 8,
    [timeFieldName]: times[0],
    [metaFieldName]: "C",
    f: 101,
};
const doc_id_9_time_f106 = {
    _id: 9,
    [timeFieldName]: times[1],
    [metaFieldName]: "C",
    f: 106
};
const doc_id_10_time_nofields = {
    _id: 10,
    [timeFieldName]: times[2],
    [metaFieldName]: "C",
};
const doc_id_11_time_nofields = {
    _id: 11,
    [timeFieldName]: times[3],
    [metaFieldName]: "C",
};
const doc_id_12_time_nometa = {
    _id: 11,
    [timeFieldName]: times[3],
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

/**
 * Tests fixed buckets optimization, which removes the residualFilter for predicates on the
 * timeField that align with the bucket boundaries.
 */
const fixedBucketOptions = {
    bucketMaxSpanSeconds: bucketParam,
    bucketRoundingSeconds: bucketParam
};

// Since the predicate aligns with bucket boundaries, we expect no residualFilter and a full bucket
// deletion.
(function testFullBucketDelete_NoFilter() {
    testDelete({
        initialDocList: [
            doc_id_8_time_f101,
            doc_id_9_time_f106,
            doc_id_10_time_nofields,
            doc_id_11_time_nofields
        ],
        deleteList: [{
            q: {[timeFieldName]: {$lt: times[3]}},
            limit: 0,
        }],
        resultDocList: [doc_id_11_time_nofields],
        nDeleted: 3,
        timeseriesOptions: fixedBucketOptions
    });
})();

// Since the predicate is a conjunction on the metaField and timeField, and only uses $gte, we
// expect no residual filter.
(function testDeleteConjunctionMeta_NoFilter() {
    testDelete({
        initialDocList: [doc_id_8_time_f101, doc_id_9_time_f106, doc_id_12_time_nometa],
        deleteList: [{
            q: {[timeFieldName]: {$gte: times[1]}, [metaFieldName]: "C"},
            limit: 0,
        }],
        resultDocList: [doc_id_8_time_f101, doc_id_12_time_nometa],
        nDeleted: 1,
        timeseriesOptions: fixedBucketOptions
    });
})();

// Since the predicate is $lte, we expect a residualFilter even though the predicate aligns with the
// bucket boundaries.
(function testPartialBucketDelete_Filter() {
    testDelete({
        initialDocList: [doc_id_9_time_f106, doc_id_10_time_nofields, doc_id_11_time_nofields],
        deleteList: [{
            q: {[timeFieldName]: {$lte: times[1]}},
            limit: 0,
        }],
        resultDocList: [doc_id_10_time_nofields, doc_id_11_time_nofields],
        nDeleted: 1,
        timeseriesOptions: fixedBucketOptions
    });
})();
