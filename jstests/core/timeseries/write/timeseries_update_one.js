/**
 * Tests singleton updates on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-78683: Remove this tag.
 *   # Internal transaction api might not handle stepdowns correctly and time-series retryable
 *   # updates use internal transaction api.
 *   does_not_support_stepdowns
 * ]
 */

import {
    getTestDB,
    metaFieldName,
    prepareCollection,
    testUpdateOne,
    timeFieldName
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

// The test, which works under the featureFlagTimeseriesUpdatesSupport requires the collection to be
// tracked when retriable writes are enabled.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation && TestData.sessionOptions &&
    TestData.sessionOptions.retryWrites) {
    jsTest.log(
        "When featureFlagTimeseriesUpdatesSupport is enabled, we expect the collections to be tracked in order to enable retryable writes. Skipping test.");
    quit();
}
/**
 * Tests op-style updates.
 */
{
    const doc_m1_a_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 1, _id: 1, a: 1, b: 1};
    const doc_a_b = {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), _id: 1, a: 1, b: 1};
    const doc_m1_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 1, _id: 1, b: 1};
    const doc_m2_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 2, _id: 1, b: 1};
    const doc_m1_arrayA_b = {
        [timeFieldName]: ISODate("2023-02-06T19:19:01Z"),
        [metaFieldName]: 1,
        _id: 1,
        a: ["arr", "ay"],
        b: 1
    };
    const doc_stringM1_a_b = {
        [timeFieldName]: ISODate("2023-02-06T19:19:01Z"),
        [metaFieldName]: "string",
        _id: 1,
        a: 1,
        b: 1
    };
    const doc_m1_c_d =
        {[timeFieldName]: ISODate("2023-02-06T19:19:02Z"), [metaFieldName]: 1, _id: 2, c: 1, d: 1};
    const query_m1_a1 = {a: {$eq: 1}, [metaFieldName]: {$eq: 1}};
    const query_m1_b1 = {b: {$eq: 1}, [metaFieldName]: {$eq: 1}};

    // Metric field update: unset field.
    (function testUnsetMetricField() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: query_m1_a1,
            updateObj: {$unset: {a: ""}},
            resultDocList: [doc_m1_b, doc_m1_c_d],
            nMatched: 1
        });
    })();

    // Metric field update: add new field.
    (function testAddNewMetricField() {
        testUpdateOne({
            initialDocList: [doc_m1_b, doc_m1_c_d],
            updateQuery: query_m1_b1,
            updateObj: {$set: {a: 1}},
            resultDocList: [doc_m1_a_b, doc_m1_c_d],
            nMatched: 1
        });
    })();

    // Metric field update: change field type (integer to array).
    (function testChangeFieldType() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: query_m1_a1,
            updateObj: {$set: {a: ["arr", "ay"]}},
            resultDocList: [doc_m1_arrayA_b, doc_m1_c_d],
            nMatched: 1
        });
    })();

    // Metric field update: no-op with non-existent field to unset.
    (function testMatchOneNoopUpdate() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: query_m1_a1,
            updateObj: {$unset: {z: ""}},
            resultDocList: [doc_m1_a_b, doc_m1_c_d],
            nMatched: 1,
            nModified: 0
        });
    })();

    // Metric field update: no-op with non-existent field to unset.
    (function testMatchMultipleNoopUpdate() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: {},
            updateObj: {$unset: {z: ""}},
            resultDocList: [doc_m1_a_b, doc_m1_c_d],
            nMatched: 1,
            nModified: 0
        });
    })();

    // Metric field update: match multiple docs, only update one.
    (function testMatchMultipleUpdateOne() {
        testUpdateOne({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
            updateQuery: {},
            updateObj: {$set: {a: 100}},
            // Don't validate exact results as we could update any doc.
            nMatched: 1,
        });
    })();

    // Match and update zero docs.
    (function testMatchNone() {
        testUpdateOne({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
            updateQuery: {[metaFieldName]: {z: "Z"}},
            updateObj: {$set: {a: 100}},
            resultDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
            nMatched: 0,
        });
    })();

    // Match and update zero docs.
    (function testMatchNoneWithCaseSensitiveCollation() {
        testUpdateOne({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_stringM1_a_b],
            updateQuery: {[metaFieldName]: "STRING"},
            collation: {locale: "en", strength: 3},
            updateObj: {$unset: {[metaFieldName]: ""}},
            resultDocList: [doc_a_b, doc_m1_a_b, doc_stringM1_a_b],
            nMatched: 0,
        });
    })();

    // Match and update 1 doc with insensitive collation.
    (function testMatchOneWithCaseInsensitiveCollation() {
        testUpdateOne({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_stringM1_a_b],
            updateQuery: {[metaFieldName]: "STRING"},
            collation: {locale: "en", strength: 1},
            updateObj: {$unset: {[metaFieldName]: ""}},
            resultDocList: [doc_a_b, doc_m1_a_b, doc_a_b],
            nMatched: 1,
        });
    })();

    // Meta-only update only updates one.
    (function testMetaOnlyUpdateOne() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: {[metaFieldName]: 1},
            updateObj: {$set: {[metaFieldName]: 2}},
            // Don't validate exact results as we could update any doc.
            nMatched: 1,
        });
    })();

    // Meta field update: remove meta field.
    (function testRemoveMetaField() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: query_m1_a1,
            updateObj: {$unset: {[metaFieldName]: ""}},
            resultDocList: [doc_a_b, doc_m1_c_d],
            nMatched: 1
        });
    })();

    // Meta field update: add meta field.
    (function testAddMetaField() {
        testUpdateOne({
            initialDocList: [doc_a_b],
            updateQuery: {},
            updateObj: {$set: {[metaFieldName]: 1}},
            resultDocList: [doc_m1_a_b],
            nMatched: 1
        });
    })();

    // Meta field update: update meta field.
    (function testUpdateMetaField() {
        testUpdateOne({
            initialDocList: [doc_m1_b],
            updateQuery: {},
            updateObj: {$set: {[metaFieldName]: 2}},
            resultDocList: [doc_m2_b],
            nMatched: 1
        });
    })();

    // Meta field update: update meta field to different type (integer to string).
    (function testChangeMetaFieldType() {
        testUpdateOne({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            updateQuery: query_m1_a1,
            updateObj: {$set: {[metaFieldName]: "string"}},
            resultDocList: [doc_stringM1_a_b, doc_m1_c_d],
            nMatched: 1
        });
    })();
}

/**
 * Tests pipeline-style updates.
 */
{
    const timestamp2023 = ISODate("2023-02-06T19:19:00Z");
    const timestamp2022 = ISODate("2022-02-06T19:19:00Z");
    const doc_2023_m1_a1 = {[timeFieldName]: timestamp2023, [metaFieldName]: 1, _id: 1, a: 1};
    const doc_2022_m2_a1_newField =
        {[timeFieldName]: timestamp2022, [metaFieldName]: 2, _id: 1, a: 1, "newField": 42};

    // Update timeField, metaField and add a new field.
    // Skip tests changing the shard key value in sharding.
    if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
        (function testPipelineUpdateSetMultipleFields() {
            testUpdateOne({
                initialDocList: [doc_2023_m1_a1],
                updateQuery: {a: {$eq: 1}, [metaFieldName]: {$eq: 1}},
                updateObj: [
                    {$set: {[timeFieldName]: timestamp2022}},
                    {$set: {[metaFieldName]: 2}},
                    {$set: {"newField": 42}},
                ],
                resultDocList: [doc_2022_m2_a1_newField],
                nMatched: 1
            });
        })();
    }

    // Expect removal of the timeField to fail.
    (function testRemoveTimeField() {
        testUpdateOne({
            initialDocList: [doc_2023_m1_a1],
            updateQuery: {},
            updateObj: [{$set: {[metaFieldName]: 2}}, {$unset: timeFieldName}],
            resultDocList: [doc_2023_m1_a1],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Expect changing the type of the timeField to fail.
    (function testChangeTimeFieldType() {
        testUpdateOne({
            initialDocList: [doc_2023_m1_a1],
            updateQuery: {},
            updateObj: [{$set: {[timeFieldName]: "string"}}],
            resultDocList: [doc_2023_m1_a1],
            failCode: ErrorCodes.BadValue,
        });
    })();
}

/**
 * Tests full measurement replacement.
 */
{
    const timestamp2023 = ISODate("2023-02-06T19:19:00Z");
    const timestamp2022 = ISODate("2022-02-06T19:19:00Z");
    const doc_t2023_m1_id1_a1 = {[timeFieldName]: timestamp2023, [metaFieldName]: 1, _id: 1, a: 1};
    const doc_t2022_m2_id2_a2 = {[timeFieldName]: timestamp2022, [metaFieldName]: 2, _id: 2, a: 2};
    const doc_t2022_m2_noId_a2 = {[timeFieldName]: timestamp2022, [metaFieldName]: 2, a: 2};

    // Skip tests changing the shard key value in sharding.
    if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
        // Full measurement replacement: update every field in the document, including the _id.
        (function testReplacementUpdateChangeId() {
            testUpdateOne({
                initialDocList: [doc_t2023_m1_id1_a1],
                updateQuery: {},
                updateObj: doc_t2022_m2_id2_a2,
                resultDocList: [doc_t2022_m2_id2_a2],
                nMatched: 1
            });
        })();

        // Full measurement replacement: update every field in the document, except the _id.
        (function testReplacementUpdateNoId() {
            testUpdateOne({
                initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
                updateQuery: {_id: 1},
                updateObj: doc_t2022_m2_noId_a2,
                resultDocList: [
                    doc_t2022_m2_id2_a2,
                    {[timeFieldName]: timestamp2022, [metaFieldName]: 2, a: 2, _id: 1},
                ],
                nMatched: 1
            });
        })();

        // Replacement that results in two duplicate measurements.
        (function testReplacementUpdateDuplicateIds() {
            testUpdateOne({
                initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
                updateQuery: {_id: 1},
                updateObj: doc_t2022_m2_id2_a2,
                resultDocList: [doc_t2022_m2_id2_a2, doc_t2022_m2_id2_a2],
                nMatched: 1,
            });
        })();
    }

    // Replacement with no time field.
    (function testReplacementUpdateNoTimeField() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            updateQuery: {_id: 1},
            updateObj: {[metaFieldName]: 1, a: 1, _id: 10},
            resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Replacement with time field of the wrong type.
    (function testReplacementUpdateWrongTypeTimeField() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            updateQuery: {_id: 1},
            updateObj: {[metaFieldName]: 1, a: 1, _id: 10, [timeFieldName]: "string"},
            resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Replacement that only references the meta field. Still fails because of the missing time
    // field.
    (function testReplacementMetaOnly() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            updateQuery: {[metaFieldName]: 1},
            updateObj: {[metaFieldName]: 3},
            resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Tests upsert with full measurement.
    (function testUpsert() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1],
            updateQuery: {[metaFieldName]: {$eq: 2}},
            updateObj: doc_t2022_m2_id2_a2,
            resultDocList: [doc_t2023_m1_id1_a1],
            upsert: true,
            upsertedDoc: doc_t2022_m2_id2_a2,
        });
    })();

    // Tests upsert with full measurement: no-op when the query matches but update is a no-op.
    (function testNoopUpsert() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1],
            updateQuery: {},
            updateObj: {$unset: {z: ""}},
            resultDocList: [doc_t2023_m1_id1_a1],
            nMatched: 1,
            nModified: 0,
            upsert: true
        });
    })();

    // Run a replacement upsert that includes an _id in the query.
    (function testReplacementUpsertWithId() {
        testUpdateOne({
            initialDocList: [doc_t2023_m1_id1_a1],
            updateQuery: {_id: 100},
            updateObj: {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), a: 5},
            upsert: true,
            upsertedDoc: {_id: 100, [timeFieldName]: ISODate("2023-02-06T19:19:01Z"), a: 5},
            resultDocList: [doc_t2023_m1_id1_a1],
        });
    })();
}

/**
 * Tests upsert with multi:false.
 */
{
    const dateTime = ISODate("2021-07-12T16:00:00Z");
    const dateTimeUpdated = ISODate("2023-01-27T16:00:00Z");
    const doc_id_1_a_b_no_metrics = {
        _id: 1,
        [timeFieldName]: dateTime,
        [metaFieldName]: {a: "A", b: "B"},
    };

    // Run an upsert that doesn't include an _id.
    (function testUpsertWithNoId() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[metaFieldName]: {z: "Z"}},
            updateObj: {$set: {[timeFieldName]: dateTime}},
            upsert: true,
            upsertedDoc: {[metaFieldName]: {z: "Z"}, [timeFieldName]: dateTime},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert that includes an _id.
    (function testUpsertWithId() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {_id: 100},
            updateObj: {$set: {[timeFieldName]: dateTime}},
            upsert: true,
            upsertedDoc: {_id: 100, [timeFieldName]: dateTime},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert that updates documents and skips the upsert.
    (function testUpsertUpdatesDocs() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[metaFieldName + ".a"]: "A"},
            updateObj: {$set: {f: 10}},
            upsert: true,
            resultDocList: [
                {
                    _id: 1,
                    [timeFieldName]: dateTime,
                    [metaFieldName]: {a: "A", b: "B"},
                    f: 10,
                },
            ],
            nMatched: 1,
        });
    })();

    // Run an upsert that matches a bucket but no documents in it, and inserts the document into a
    // bucket with the same parameters.
    (function testUpsertIntoMatchedBucket() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[metaFieldName]: {a: "A", b: "B"}, f: 111},
            updateObj: {$set: {[timeFieldName]: dateTime}},
            upsert: true,
            upsertedDoc: {[metaFieldName]: {a: "A", b: "B"}, [timeFieldName]: dateTime, f: 111},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert that doesn't insert a time field.
    (function testUpsertNoTimeField() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[metaFieldName]: {z: "Z"}},
            updateObj: {$set: {f: 10}},
            upsert: true,
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Run an upsert where the time field is provided in the query.
    (function testUpsertQueryOnTimeField() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[timeFieldName]: dateTimeUpdated},
            updateObj: {$set: {f: 10}},
            upsert: true,
            upsertedDoc: {[timeFieldName]: dateTimeUpdated, f: 10},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert where a document to insert is supplied by the request.
    (function testUpsertSupplyDoc() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[timeFieldName]: dateTimeUpdated},
            updateObj: [{$set: {f: 10}}],
            upsert: true,
            upsertedDoc: {[timeFieldName]: dateTime, f: 100},
            c: {new: {[timeFieldName]: dateTime, f: 100}},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert where a document to insert is supplied by the request and does not have a time
    // field.
    (function testUpsertSupplyDocNoTimeField() {
        testUpdateOne({
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateQuery: {[timeFieldName]: dateTimeUpdated},
            updateObj: [{$set: {f: 10}}],
            upsert: true,
            c: {new: {[metaFieldName]: {a: "A"}, f: 100}},
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
            failCode: ErrorCodes.BadValue,
        });
    })();
}

/**
 * Tests measurement modification that could exceed bucket size limit (default value of 128000
 * bytes).
 */
(function testUpdateExceedsBucketSizeLimit() {
    const testDB = getTestDB();
    const collName = "testUpdateExceedsBucketSizeLimit";
    const coll = testDB.getCollection(collName);
    prepareCollection({collName, initialDocList: []});

    // Fill up a bucket to roughly 120000 bytes by inserting 4 batches of 30 documents sized at
    // 1000 bytes.
    let batchNum = 0;
    while (batchNum < 4) {
        let batch = [];
        for (let i = 0; i < 30; i++) {
            const doc =
                {_id: i, [timeFieldName]: ISODate("2023-07-13T17:00:00Z"), value: "a".repeat(1000)};
            batch.push(doc);
        }

        assert.commandWorked(coll.insertMany(batch), {ordered: false});
        batchNum++;
    }

    // Update any of the measurements with a document which will exceed the 128000 byte threshold.
    const chunkyDoc =
        {_id: 128000, [timeFieldName]: ISODate("2023-07-13T17:00:00Z"), value: "a".repeat(10000)};

    const updateCommand = {update: collName, updates: [{q: {}, u: chunkyDoc, multi: false}]};
    const res = assert.commandWorked(testDB.runCommand(updateCommand));
    assert.eq(1, res.n, tojson(res));
    assert.eq(1, res.nModified, tojson(res));
})();
