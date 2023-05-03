/**
 * Tests running the deleteOne command on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

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
