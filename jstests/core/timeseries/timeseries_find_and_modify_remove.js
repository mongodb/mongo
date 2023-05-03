/**
 * Tests findAndModify with remove: true on a timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # findAndModify with remove: true on a timeseries collection is supported since 7.1
 *   requires_fcv_71,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   # TODO SERVER-76530: Remove the follow tag.
 *   assumes_unsharded_collection,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

// findAndModify with a sort option is not supported.
(function testSortOptionFails() {
    jsTestLog("Running testSortOptionFails()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc4_b_f103, doc6_c_f105],
        cmd: {filter: {f: {$gt: 100}}, sort: {f: 1}},
        res: {errorCode: ErrorCodes.InvalidOptions},
    });
})();

// Query on the 'f' field leads to zero measurement delete.
(function testZeroMeasurementDelete() {
    jsTestLog("Running testZeroMeasurementDelete()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc4_b_f103, doc6_c_f105],
        cmd: {filter: {f: 17}},
        res: {
            expectedDocList: [doc1_a_nofields, doc4_b_f103, doc6_c_f105],
            nDeleted: 0,
            bucketFilter: makeBucketFilter({
                $and: [
                    {"control.min.f": {$_internalExprLte: 17}},
                    {"control.max.f": {$_internalExprGte: 17}},
                ]
            }),
            residualFilter: {f: {$eq: 17}},
            nBucketsUnpacked: 0,
            nReturned: 0,
        },
    });
})();

// Query on the 'f' field leads to a partial bucket delete.
(function testPartialBucketDelete() {
    jsTestLog("Running testPartialBucketDelete()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        cmd: {filter: {f: 101}},
        res:
            {expectedDocList: [doc1_a_nofields, doc3_a_f102], nDeleted: 1, deletedDoc: doc2_a_f101},
    });
})();

// Query on the 'f' field leads to a partial bucket delete and 'fields' project the returned doc.
(function testPartialBucketDeleteWithFields() {
    jsTestLog("Running testPartialBucketDeleteWithFields()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        cmd: {filter: {f: 102}, fields: {f: 1, [metaFieldName]: 1, _id: 0}},
        res: {
            expectedDocList: [doc1_a_nofields, doc2_a_f101],
            nDeleted: 1,
            deletedDoc: {f: 102, [metaFieldName]: "A"},
            rootStage: "PROJECTION_DEFAULT",
            bucketFilter: makeBucketFilter({
                $and: [
                    {"control.min.f": {$_internalExprLte: 102}},
                    {"control.max.f": {$_internalExprGte: 102}},
                ]
            }),
            residualFilter: {f: {$eq: 102}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();

// Query on the 'f' field leads to a full (single document) bucket delete.
(function testFullBucketDelete() {
    jsTestLog("Running testFullBucketDelete()");
    testFindOneAndRemove({
        initialDocList: [doc2_a_f101],
        cmd: {filter: {f: 101}},
        res: {
            expectedDocList: [],
            nDeleted: 1,
            deletedDoc: doc2_a_f101,
            bucketFilter: makeBucketFilter({
                $and: [
                    {"control.min.f": {$_internalExprLte: 101}},
                    {"control.max.f": {$_internalExprGte: 101}},
                ]
            }),
            residualFilter: {f: {$eq: 101}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();

// Query on the 'tag' field matches all docs and deletes one.
(function testMatchFullBucketOnlyDeletesOne() {
    jsTestLog("Running testMatchFullBucketOnlyDeletesOne()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        cmd: {filter: {[metaFieldName]: "A"}},
        // Don't validate exact results as we could delete any doc.
        res: {
            nDeleted: 1,
            bucketFilter: makeBucketFilter({meta: {$eq: "A"}}),
            residualFilter: {},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();

// Query on the 'tag' and metric field.
(function testMetaAndMetricFilterOnlyDeletesOne() {
    jsTestLog("Running testMetaAndMetricFilterOnlyDeletesOne()");
    testFindOneAndRemove({
        initialDocList: [doc1_a_nofields, doc2_a_f101, doc3_a_f102],
        cmd: {filter: {[metaFieldName]: "A", f: {$gt: 101}}},
        res: {
            nDeleted: 1,
            deletedDoc: doc3_a_f102,
            bucketFilter:
                makeBucketFilter({meta: {$eq: "A"}}, {"control.max.f": {$_internalExprGt: 101}}),
            residualFilter: {f: {$gt: 101}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        }
    });
})();

// Query on the 'f' field matches docs in multiple buckets but only deletes from one.
(function testMatchMultiBucketOnlyDeletesOne() {
    jsTestLog("Running testMatchMultiBucketOnlyDeletesOne()");
    testFindOneAndRemove({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        cmd: {filter: {f: {$gt: 101}}},
        // Don't validate exact results as we could delete one of a few docs.
        res: {
            nDeleted: 1,
            bucketFilter: makeBucketFilter({"control.max.f": {$_internalExprGt: 101}}),
            residualFilter: {f: {$gt: 101}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();

// Empty filter matches all docs but only deletes one.
(function testEmptyFilterOnlyDeletesOne() {
    jsTestLog("Running testEmptyFilterOnlyDeletesOne()");
    testFindOneAndRemove({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        cmd: {filter: {}},
        // Don't validate exact results as we could delete any doc.
        res: {
            nDeleted: 1,
            bucketFilter: makeBucketFilter({}),
            residualFilter: {},
            nBucketsUnpacked: 1,
            nReturned: 1
        },
    });
})();

// Verifies that the collation is properly propagated to the bucket-level filter when the
// query-level collation overrides the collection default collation.
(function testFindAndRemoveWithCollation() {
    jsTestLog("Running testFindAndRemoveWithCollation()");
    testFindOneAndRemove({
        initialDocList: [
            doc1_a_nofields,
            doc2_a_f101,
            doc3_a_f102,
            doc4_b_f103,
            doc5_b_f104,
            doc6_c_f105,
            doc7_c_f106
        ],
        cmd: {
            filter: {[metaFieldName]: "a", f: {$gt: 101}},
            /*caseInsensitive collation*/
            collation: {locale: "en", strength: 2}
        },
        res: {
            nDeleted: 1,
            deletedDoc: doc3_a_f102,
            bucketFilter:
                makeBucketFilter({meta: {$eq: "a"}}, {"control.max.f": {$_internalExprGt: 101}}),
            residualFilter: {f: {$gt: 101}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();
})();
