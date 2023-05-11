/**
 * Tests findAndModify remove on a sharded timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   # findAndModify remove on a sharded timeseries collection is supported since 7.1
 *   requires_fcv_71,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

setUpShardedCluster();

(function testSortOptionFailsOnShardedCollection() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {f: {$gt: 100}}, sort: {f: 1}},
        res: {errorCode: ErrorCodes.InvalidOptions},
    });
})();

(function testProjectOptionHonoredOnShardedCollection() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {f: 106}, fields: {_id: 1, [timeFieldName]: 1, f: 1}},
        res: {
            nDeleted: 1,
            deletedDoc: {
                _id: doc7_c_f106._id,
                [timeFieldName]: doc7_c_f106[timeFieldName],
                f: doc7_c_f106.f
            },
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

// Verifies that the collation is properly propagated to the bucket-level filter when the
// query-level collation overrides the collection default collation. This is a two phase delete due
// to the user-specified collation.
(function testTwoPhaseDeleteCanHonorCollationOnShardedCollection() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {[metaFieldName]: "a", f: {$gt: 101}},
            // caseInsensitive collation
            collation: {locale: "en", strength: 2}
        },
        res: {
            nDeleted: 1,
            deletedDoc: doc3_a_f102,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "primary",
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted delete but no measurement is deleted.
(function testTargetedDeleteByNonMatchingFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[metaFieldName]: "C", f: 17}},
        res: {nDeleted: 0, writeType: "targeted", dataBearingShard: "other"},
    });
})();

// Query on the 'f' field leads to zero measurement delete.
(function testTwoPhaseDeleteByNonMatchingFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {f: 17}},
        res: {nDeleted: 0, writeType: "twoPhaseProtocol", dataBearingShard: "none"},
    });
})();

// Query on the meta field and 'f' field leads to a targeted delete when the meta field is included
// in the shard key.
(function testTargetedDeleteByShardKeyAndFieldFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[metaFieldName]: "B", f: 103}},
        res: {
            nDeleted: 1,
            deletedDoc: doc4_b_f103,
            writeType: "targeted",
            dataBearingShard: "other",
        },
    });
})();

// Query on the meta field and 'f' field leads to a two phase delete when the meta field is not
// included in the shard key.
(function testTwoPhaseDeleteByMetaAndFieldFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        includeMeta: false,
        cmd: {filter: {[metaFieldName]: "B", f: 103}},
        res: {
            nDeleted: 1,
            deletedDoc: doc4_b_f103,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted delete when the meta field is included
// in the shard key.
(function testTargetedDeleteByShardKeyAndFieldRangeFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[metaFieldName]: "A", f: {$lt: 103}}},
        res: {
            nDeleted: 1,
            deletedDoc: doc2_a_f101,
            writeType: "targeted",
            dataBearingShard: "primary",
        },
    });
})();

// Query on the meta field and 'f' field leads to a two phase delete when the meta field is not
// included in the shard key.
(function testTwoPhaseDeleteByShardKeyAndFieldRangeFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        includeMeta: false,
        cmd: {filter: {[metaFieldName]: "A", f: {$lt: 103}}},
        res: {
            nDeleted: 1,
            deletedDoc: doc2_a_f101,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "primary",
        },
    });
})();

// Query on the time field leads to a targeted delete when the time field is included in the shard
// key.
(function testTargetedDeleteByTimeShardKeyFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        includeMeta: false,
        cmd: {filter: {[timeFieldName]: doc6_c_f105[timeFieldName]}},
        res: {
            nDeleted: 1,
            deletedDoc: doc6_c_f105,
            writeType: "targeted",
            dataBearingShard: "other",
        },
    });
})();

// Query on the time field leads to a two phase delete when the time field is not included in the
// shard key.
(function testTwoPhaseDeleteByTimeFieldFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[timeFieldName]: doc7_c_f106[timeFieldName]}},
        res: {
            nDeleted: 1,
            deletedDoc: doc7_c_f106,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

// Empty filter matches all docs but only deletes one.
(function testTwoPhaseDeleteByEmptyFilter() {
    testFindOneAndRemoveOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {}},
        // Don't validate exact results as we could delete any doc from any shard.
        res: {nDeleted: 1, writeType: "twoPhaseProtocol", dataBearingShard: "any"},
    });
})();

tearDownShardedCluster();
})();
