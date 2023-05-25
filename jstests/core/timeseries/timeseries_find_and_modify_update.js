/**
 * Tests singleton updates on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-76871 Remove this tag.
 *   assumes_unsharded_collection,
 *   # TODO SERVER-76454 Remove the following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

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
        [metaFieldName]: "1",
        _id: 1,
        a: 1,
        b: 1
    };
    const doc_m1_c_d =
        {[timeFieldName]: ISODate("2023-02-06T19:19:02Z"), [metaFieldName]: 1, _id: 2, c: 1, d: 1};
    const query_m1_a1 = {a: {$eq: 1}, [metaFieldName]: {$eq: 1}};
    const query_m1_b1 = {b: {$eq: 1}, [metaFieldName]: {$eq: 1}};

    // Verifies that sort option is rejected.
    (function testSortOptionFails() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: {},
                update: {$unset: {a: ""}},
                sort: {_id: 1},
            },
            res: {errorCode: ErrorCodes.InvalidOptions},
        });
    })();

    // Metric field update: unset field and return the old doc.
    (function testUnsetMetricField() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$unset: {a: ""}},
            },
            res: {
                resultDocList: [doc_m1_b, doc_m1_c_d],
                returnDoc: doc_m1_a_b,
                bucketFilter: makeBucketFilter({meta: {$eq: 1}}, {
                    $and: [
                        {"control.min.a": {$_internalExprLte: 1}},
                        {"control.max.a": {$_internalExprGte: 1}}
                    ]
                }),
                residualFilter: {a: {$eq: 1}},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
        });
    })();

    // Metric field update: add new field and return the new doc.
    (function testAddNewMetricField() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_b, doc_m1_c_d],
            cmd: {filter: query_m1_b1, update: {$set: {a: 1}}, returnNew: true},
            res: {
                resultDocList: [doc_m1_a_b, doc_m1_c_d],
                returnDoc: doc_m1_a_b,
                bucketFilter: makeBucketFilter({meta: {$eq: 1}}, {
                    $and: [
                        {"control.min.b": {$_internalExprLte: 1}},
                        {"control.max.b": {$_internalExprGte: 1}}
                    ]
                }),
                residualFilter: {b: {$eq: 1}},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
        });
    })();

    // Metric field update: change field type (integer to array) with 'fields' option.
    (function testChangeFieldTypeWithFields() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$set: {a: ["arr", "ay"]}},
                fields: {a: 1, b: 1, _id: 0},
            },
            res: {resultDocList: [doc_m1_arrayA_b, doc_m1_c_d], returnDoc: {a: 1, b: 1}},
        });
    })();

    // Metric field update: no-op with non-existent field to unset.
    (function testMatchOneNoopUpdate() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$unset: {z: ""}},
            },
            res: {
                resultDocList: [doc_m1_a_b, doc_m1_c_d],
                returnDoc: doc_m1_a_b,
            },
        });
    })();

    // Metric field update: no-op with non-existent field to unset and returnNew.
    (function testMatchOneNoopUpdateWithReturnNew() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$unset: {z: ""}},
                returnNew: true,
            },
            res: {
                resultDocList: [doc_m1_a_b, doc_m1_c_d],
                // The return doc is the same as the original doc, since the update is a no-op.
                returnDoc: doc_m1_a_b,
            },
        });
    })();

    // Metric field update: no-op with non-existent field to unset.
    (function testMatchMultipleNoopUpdate() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: {},
                update: {$unset: {z: ""}},
            },
            res: {
                resultDocList: [doc_m1_a_b, doc_m1_c_d],
                returnDoc: doc_m1_a_b,
                bucketFilter: makeBucketFilter({}),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 0,
                nUpserted: 0,
            },
        });
    })();

    // Metric field update: match multiple docs, only update one, returning the new doc.
    (function testMatchMultipleUpdateOne() {
        const resultDoc = Object.assign({}, doc_a_b, {a: 100});
        testFindOneAndUpdate({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: {},
                update: {$set: {a: 100}},
                returnNew: true,
            },
            res: {
                resultDocList: [resultDoc, doc_m1_a_b, doc_m1_c_d],
                returnDoc: resultDoc,
                bucketFilter: makeBucketFilter({}),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
        });
    })();

    // Match and update zero docs.
    (function testMatchNone() {
        testFindOneAndUpdate({
            initialDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: {[metaFieldName]: {z: "Z"}},
                update: {$set: {a: 100}},
            },
            res: {
                resultDocList: [doc_a_b, doc_m1_a_b, doc_m1_c_d],
                bucketFilter: makeBucketFilter({meta: {$eq: {z: "Z"}}}),
                residualFilter: {},
                nBucketsUnpacked: 0,
                nMatched: 0,
                nModified: 0,
                nUpserted: 0,
            },
        });
    })();

    // Meta-only update only updates one.
    (function testMetaOnlyUpdateOne() {
        const returnDoc = Object.assign({}, doc_m1_a_b, {[metaFieldName]: 2});
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: {[metaFieldName]: 1},
                update: {$set: {[metaFieldName]: 2}},
                returnNew: true,
            },
            res: {
                resultDocList: [doc_m1_c_d, returnDoc],
                returnDoc: returnDoc,
                bucketFilter: makeBucketFilter({meta: {$eq: 1}}),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
                nUpserted: 0,
            },
        });
    })();

    // Meta field update: remove meta field.
    (function testRemoveMetaField() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$unset: {[metaFieldName]: ""}},
            },
            res: {resultDocList: [doc_a_b, doc_m1_c_d], returnDoc: doc_m1_a_b},
        });
    })();

    // Meta field update: add meta field.
    (function testAddMetaField() {
        testFindOneAndUpdate({
            initialDocList: [doc_a_b],
            cmd: {
                filter: {},
                update: {$set: {[metaFieldName]: 1}},
            },
            res: {resultDocList: [doc_m1_a_b], returnDoc: doc_a_b},
        });
    })();

    // Meta field update: update meta field.
    (function testUpdateMetaField() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_b],
            cmd: {
                filter: {},
                update: {$set: {[metaFieldName]: 2}},
            },
            res: {resultDocList: [doc_m2_b], returnDoc: doc_m1_b},
        });
    })();

    // Meta field update: update meta field to different type (integer to string).
    (function testChangeMetaFieldType() {
        testFindOneAndUpdate({
            initialDocList: [doc_m1_a_b, doc_m1_c_d],
            cmd: {
                filter: query_m1_a1,
                update: {$set: {[metaFieldName]: "1"}},
            },
            res: {resultDocList: [doc_stringM1_a_b, doc_m1_c_d], returnDoc: doc_m1_a_b},
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
    (function testPipelineUpdateSetMultipleFields() {
        testFindOneAndUpdate({
            initialDocList: [doc_2023_m1_a1],
            cmd: {
                filter: {a: {$eq: 1}, [metaFieldName]: {$eq: 1}},
                update: [
                    {$set: {[timeFieldName]: timestamp2022}},
                    {$set: {[metaFieldName]: 2}},
                    {$set: {"newField": 42}},
                ],
            },
            res: {
                resultDocList: [doc_2022_m2_a1_newField],
                returnDoc: doc_2023_m1_a1,
                bucketFilter: makeBucketFilter({meta: {$eq: 1}}, {
                    $and: [
                        {"control.min.a": {$_internalExprLte: 1}},
                        {"control.max.a": {$_internalExprGte: 1}},
                    ]
                }),
                residualFilter: {a: {$eq: 1}},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
                nUpserted: 0,
            },
        });
    })();

    // Expect removal of the timeField to fail.
    (function testPipelineRemoveTimeField() {
        testFindOneAndUpdate({
            initialDocList: [doc_2023_m1_a1],
            cmd: {
                filter: {},
                update: [{$set: {[metaFieldName]: 2}}, {$unset: timeFieldName}],
            },
            res: {
                errorCode: ErrorCodes.BadValue,
                resultDocList: [doc_2023_m1_a1],
            },
        });
    })();

    // Expect changing the type of the timeField to fail.
    (function testPipelineChangeTimeFieldType() {
        testFindOneAndUpdate({
            initialDocList: [doc_2023_m1_a1],
            cmd: {
                filter: {},
                update: [{$set: {[timeFieldName]: "string"}}],
            },
            res: {
                errorCode: ErrorCodes.BadValue,
                resultDocList: [doc_2023_m1_a1],
            },
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

    // Full measurement replacement: update every field in the document, including the _id.
    (function testReplacementUpdateChangeId() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1],
            cmd: {
                filter: {},
                update: doc_t2022_m2_id2_a2,
            },
            res: {resultDocList: [doc_t2022_m2_id2_a2], returnDoc: doc_t2023_m1_id1_a1},
        });
    })();

    // Full measurement replacement: update every field in the document, except the _id.
    (function testReplacementUpdateNoId() {
        const returnDoc = {[timeFieldName]: timestamp2022, [metaFieldName]: 2, a: 2, _id: 1};
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            cmd: {
                filter: {_id: 1},
                update: doc_t2022_m2_noId_a2,
                returnNew: true,
            },
            res: {
                resultDocList: [
                    doc_t2022_m2_id2_a2,
                    returnDoc,
                ],
                returnDoc: returnDoc,
            },
        });
    })();

    // Replacement with no time field.
    (function testReplacementUpdateNoTimeField() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            cmd: {
                filter: {_id: 1},
                update: {[metaFieldName]: 1, a: 1, _id: 10},
            },
            res: {
                errorCode: ErrorCodes.BadValue,
                resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            }
        });
    })();

    // Replacement with time field of the wrong type.
    (function testReplacementUpdateWrongTypeTimeField() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            cmd: {
                filter: {_id: 1},
                update: {[metaFieldName]: 1, a: 1, _id: 10, [timeFieldName]: "string"},
            },
            res: {
                errorCode: ErrorCodes.BadValue,
                resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            },
        });
    })();

    // Replacement that results in two duplicate measurements.
    (function testReplacementUpdateDuplicateIds() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            cmd: {
                filter: {_id: 1},
                update: doc_t2022_m2_id2_a2,
            },
            res: {
                resultDocList: [doc_t2022_m2_id2_a2, doc_t2022_m2_id2_a2],
                returnDoc: doc_t2023_m1_id1_a1,
            },
        });
    })();

    // Replacement that only references the meta field. Still fails because of the missing time
    // field.
    (function testReplacementMetaOnly() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            cmd: {
                filter: {[metaFieldName]: 1},
                update: {[metaFieldName]: 3},
            },
            res: {
                errorCode: ErrorCodes.BadValue,
                resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
            },
        });
    })();

    // Tests upsert with full measurement & returnNew = false.
    (function testUpsert() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1],
            cmd: {
                filter: {[metaFieldName]: {$eq: 2}},
                update: doc_t2022_m2_id2_a2,
                // returnNew defaults to false.
                upsert: true,
            },
            res: {
                resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
                returnDoc: null,
                bucketFilter: makeBucketFilter({meta: {$eq: 2}}),
                residualFilter: {},
                nBucketsUnpacked: 0,
                nMatched: 0,
                nModified: 0,
                nUpserted: 1,
            },
        });
    })();

    // Tests upsert with full measurement & returnNew = true.
    (function testUpsertWithReturnNew() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1],
            cmd: {
                filter: {[metaFieldName]: {$eq: 2}},
                update: doc_t2022_m2_id2_a2,
                returnNew: true,
                upsert: true,
            },
            res: {
                resultDocList: [doc_t2023_m1_id1_a1, doc_t2022_m2_id2_a2],
                returnDoc: doc_t2022_m2_id2_a2,
                bucketFilter: makeBucketFilter({meta: {$eq: 2}}),
                residualFilter: {},
                nBucketsUnpacked: 0,
                nMatched: 0,
                nModified: 0,
                nUpserted: 1,
            },
        });
    })();

    // Tests upsert with full measurement: no-op when the query matches but update is a no-op.
    (function testNoopUpsert() {
        testFindOneAndUpdate({
            initialDocList: [doc_t2023_m1_id1_a1],
            cmd: {filter: {}, update: {$unset: {z: ""}}, upsert: true},
            res: {
                resultDocList: [doc_t2023_m1_id1_a1],
                returnDoc: doc_t2023_m1_id1_a1,
                nUpserted: 0,
            },
        });
    })();
}
})();
