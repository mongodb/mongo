/**
 * Test the behavior of $group on time-series collections. Specifically, we are targeting rewrites
 * that replace bucket unpacking with $group over the buckets collection. Currently, only $min/$max
 * are supported for the rewrites.
 *
 * @tags: [
 *   directly_against_shardsvrs_incompatible,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_61,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

import {
    runGroupRewriteTest
} from 'jstests/core/timeseries/libs/timeseries_groupby_reorder_helpers.js';

// Test with measurement group key -- a rewrite in this situation would be wrong.
(function testNonMetaGroupKey() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, key: 2, val: 1},  // global min
        {time: t, myMeta: 1, key: 1, val: 3},  // min for key = 1
        {time: t, myMeta: 1, key: 1, val: 5},  // max for key = 1
        {time: t, myMeta: 1, key: 2, val: 7},  // global max
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$key", min: {$min: "$val"}}}, {$match: {_id: 1}}],
                        [{"_id": 1, "min": 3}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$key", max: {$max: "$val"}}}, {$match: {_id: 1}}],
                        [{"_id": 1, "max": 5}]);
})();

// Test with a constant group key. The $group rewrite applies.
(function testConstGroupKey_NoFilter() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 0},
        {time: t, myMeta: 1, val: 1},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(
        coll, docs, [{$group: {_id: null, min: {$min: "$val"}}}], [{"_id": null, "min": 0}]);
    runGroupRewriteTest(
        coll, docs, [{$group: {_id: null, max: {$max: "$val"}}}], [{"_id": null, "max": 5}]);
})();

// Test with a group key that is optimized to a constant. The $group rewrite applies.
(function testConstExprGroupKey_WithMinMaxAccumulator() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {$mod: [10, 5]}, min: {$min: "$val"}, max: {$max: "$val"}}}],
        [{_id: 0, min: 3, max: 5}]);
})();

// Test with a null group key and the collection has no metaField. The $group rewrite applies.
(function testNullGroupKey_WithMinMaxAccumulator_NoMetaField() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: null, min: {$min: "$val"}, max: {$max: "$val"}}}],
                        [{_id: null, min: 3, max: 5}],
                        true /* excludeMeta */);
})();

// With a filter on meta the group re-write does apply if the group key is const.
(function testConstGroupKey_WithFilterOnMeta() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 0},
        {time: t, myMeta: 2, val: 1},
        {time: t, myMeta: 2, val: 3},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {myMeta: 2}}, {$group: {_id: null, min: {$min: "$val"}}}],
                        [{"_id": null, "min": 1}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {myMeta: 2}}, {$group: {_id: null, max: {$max: "$val"}}}],
                        [{"_id": null, "max": 3}]);
})();

// In presence of a non-meta filter the group re-write doesn't apply even if the group key is const.
(function testConstGroupKey_WithFilterOnMeasurement() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 0, include: false},
        {time: t, myMeta: 1, val: 1, include: true},
        {time: t, myMeta: 1, val: 5, include: false},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {include: true}}, {$group: {_id: null, min: {$min: "$val"}}}],
                        [{"_id": null, "min": 1}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {include: true}}, {$group: {_id: null, max: {$max: "$val"}}}],
                        [{"_id": null, "max": 1}]);
})();

// Test with meta group key. The group re-write applies.
(function testMetaGroupKey_NoFilter() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 5},
        {time: t, myMeta: 2, val: 4},
        {time: t, myMeta: 2, val: 3},
        {time: t, myMeta: 1, val: 1},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", min: {$min: "$val"}}}, {$match: {_id: 2}}],
                        [{"_id": 2, "min": 3}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", max: {$max: "$val"}}}, {$match: {_id: 2}}],
                        [{"_id": 2, "max": 4}]);
})();

// Test with meta group key preceeded by a filter on the meta key. The re-write still applies.
(function testMetaGroupKey_WithFilterOnMeta() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 5},
        {time: t, myMeta: 2, val: 4},
        {time: t, myMeta: 2, val: 3},
        {time: t, myMeta: 1, val: 1},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {myMeta: 2}}, {$group: {_id: "$myMeta", min: {$min: "$val"}}}],
                        [{"_id": 2, "min": 3}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$match: {myMeta: 2}}, {$group: {_id: "$myMeta", max: {$max: "$val"}}}],
                        [{"_id": 2, "max": 4}]);
})();

// Test with meta group key preceeded by a filter on a measurement key. The re-write doesn't apply.
(function testMetaGroupKey_WithFilterOnMeasurement() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3, include: false},
        {time: t, myMeta: 1, val: 4, include: true},
        {time: t, myMeta: 1, val: 5, include: false},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$match: {include: true}}, {$group: {_id: "$myMeta", min: {$min: "$val"}}}],
        [{"_id": 1, "min": 4}]);
    runGroupRewriteTest(
        coll,
        docs,
        [{$match: {include: true}}, {$group: {_id: "$myMeta", max: {$max: "$val"}}}],
        [{"_id": 1, "max": 4}]);
})();

// Test SERVER-73822 fix: complex $min and $max (i.e. not just straight field refs) work correctly.
(function testMetaGroupKey_WithNonPathExpressionUnderMinMax() {
    const t = new Date();
    // min(a+b) != min(a) + min(b) and max(a+b) != max(a) + max(b)
    const docs = [
        {time: t, myMeta: 1, a: 1, b: 20},  // max(a + b)
        {time: t, myMeta: 1, a: 2, b: 10},
        {time: t, myMeta: 1, a: 3, b: 1},  // min(a + b)
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", min: {$min: {$add: ["$a", "$b"]}}}}],
                        [{"_id": 1, "min": 4}]);
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", max: {$max: {$add: ["$a", "$b"]}}}}],
                        [{"_id": 1, "max": 21}]);
})();

// Test with meta group key and a non-min/max accumulator that uses only the meta field. This query
// is _not_ eligible for the re-write w/o bucket unpacking because while it doesn't depend on any
// fields of the individual events it still depends on the number of events in a bucket.
(function testMetaGroupKey_WithNonMinMaxAccumulatorOnMeta() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", x: {$sum: "$myMeta"}}}, {$match: {_id: 1}}],
                        [{"_id": 1, "x": 2}]);
})();

// In presence of a filter $min and $max on the meta field cannot be re-written because a filter
// might end up selecting nothing in buckets with a particular meta.
(function testMetaGroupKey_WithAccumulatorOnMeta_WithFilterOnMeasurement() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, include: false},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$match: {include: true}}, {$group: {_id: "$myMeta", x: {$min: "$myMeta"}}}],
        []);
})();

// Test min on the time field (cannot rewrite $min because the control.time.min is rounded
// down).
(function testMetaGroupKey_WithMinOnTime() {
    const docs = [
        {time: ISODate("2023-07-20T23:16:47.683Z"), myMeta: 1},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", min: {$min: "$time"}}}],
                        [{_id: 1, min: ISODate("2023-07-20T23:16:47.683Z")}]);
})();

// Test max on the time field (can rewrite $max because the control.time.max is not rounded
// down).
(function testMetaGroupKey_WithMaxOnTime() {
    const docs = [
        {time: ISODate("2023-07-20T23:16:47.683Z"), myMeta: 1},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", max: {$max: "$time"}}}],
                        [{_id: 1, max: ISODate("2023-07-20T23:16:47.683Z")}]);
})();

// Test a group key that is a list of fields that are just referencing the metaField. The $group
// rewrite applies.
(function testListMetaFields_WithMinMaxAccumulator() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: {a: 2, b: 10}, val: 3},
        {time: t, myMeta: {a: 3, b: 10}, val: 4},
        {time: t, myMeta: {a: 2, b: 10}, val: 5},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{
            $group:
                {_id: {a: "$myMeta.a", b: "$myMeta.b"}, min: {$min: "$val"}, max: {$max: "$val"}}
        }],
        [{_id: {a: 3, b: 10}, min: 4, max: 4}, {_id: {a: 2, b: 10}, min: 3, max: 5}]);
})();

// Test a group key that is a list of fields with some fields referencing the metaField and some
// fields are not. The $group rewrite does not apply.
(function testListMetaAndOtherFields_WithMinMaxAccumulator() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: {a: 2, b: 10}, val: 3, string: "apple"},
        {time: t, myMeta: {a: 3, b: 10}, val: 4, string: "pear"},
        {time: t, myMeta: {a: 3, b: 10}, val: 6, string: "apple"},
        {time: t, myMeta: {a: 2, b: 10}, val: 5, string: "apple"}
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {a: "$myMeta.a", b: "$string"}, min: {$min: "$val"}, max: {$max: "$val"}}}],
        [
            {_id: {a: 2, b: "apple"}, min: 3, max: 5},
            {_id: {a: 3, b: "apple"}, min: 6, max: 6},
            {_id: {a: 3, b: "pear"}, min: 4, max: 4}
        ]);
})();

// Test a group key that is a list of fields and the collection does not have a metaField. The
// rewrite does not apply, since no fields are referencing the metaField.
(function testListOfOtherFields_NoMetaField() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: {a: 2, b: 10}, val: 3, string: "apple"},
        {time: t, myMeta: {a: 3, b: 10}, val: 4, string: "pear"},
        {time: t, myMeta: {a: 3, b: 10}, val: 6, string: "apple"},
        {time: t, myMeta: {a: 2, b: 10}, val: 5, string: "apple"}
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {a: "$myMeta.a", b: "$string"}, min: {$min: "$val"}, max: {$max: "$val"}}}],
        [
            {_id: {a: 2, b: "apple"}, min: 3, max: 5},
            {_id: {a: 3, b: "apple"}, min: 6, max: 6},
            {_id: {a: 3, b: "pear"}, min: 4, max: 4}
        ],
        true /* excludeMeta */);
})();

//
// The following tests validate the behavior of the $group rewrite with the $count accumulator.
//

// Test with a meta group key, and the $count accumulator. The rewrite should apply.
(function testMetaGroupKey_WithCountMinMaxAccumulator() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [
            {$group: {_id: "$myMeta", x: {$count: {}}, y: {$min: "$val"}, z: {$max: "$val"}}},
            {$match: {_id: 1}}
        ],
        [{"_id": 1, "x": 2, "y": 3, "z": 5}]);
})();

// Test with a meta group key, and {$sum: 1}. Since $count desugars to {$sum:1}, the rewrite should
// apply.
(function testMetaGroupKey_WithSum1Accumulator() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", x: {$sum: 1}}}, {$match: {_id: 1}}],
                        [{"_id": 1, "x": 2}]);
})();

// Test with a meta group key, and {$sum: 7}. The group rewrite doesn't apply in this case but we
// do a different optimization that takes into account that no fields should be unpacked.
(function testMetaGroupKey_WithSum7Accumulator() {
    const t = new Date();
    const docs = [
        {time: t, meta: 1, val: 3},
        {time: t, meta: 3, val: 4},
        {time: t, meta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$meta", x: {$sum: 7}}}, {$match: {_id: 1}}],
                        [{"_id": 1, "x": 2 * 7}]);
})();

// Test with a constant group key with the $count accumulator when there is no metaField. The
// rewrite should apply.
(function testConstGroupKey_WithCountAccumulator_NoMetaField() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: null, x: {$sum: 1}}}],
                        [{"_id": null, "x": 3}],
                        true /* excludeMeta */);
})();

// Test with a meta group key with the $sum accumulator. Even though the rewrite applies for $count
// which desugars to {$sum: 1}, for accumulators like {$sum: "$fieldPath"} the rewrite does not
// apply.
(function testMetaGroupKey_WithSumFieldPath() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: "$myMeta", x: {$sum: "$val"}}}],
                        [{"_id": 1, "x": 8}, {"_id": 3, "x": 4}]);
})();

// Test the $count aggregation stage. Since this stage is rewritten to a $group and $project, the
// rewrite will apply.
(function testMetaGroupKey_WithCountStage() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$group: {_id: null, min: {$min: "$val"}}}, {$count: "groupCount"}],
                        [{"groupCount": 1}],
                        true /* excludeMeta */);
})();

// Validates the $project followed by $group scenarios, where the $project may project out/modify
// the fields being used by the $group stage.
(function testMetaGroupKey_WithProjectStagePrefix() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];

    //
    // Tests with inclusion projections.
    //
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 1}}, {$group: {_id: null, o: {$max: "$time"}}}],
                        [{_id: null, o: null}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 1}}, {$group: {_id: null, o: {$max: "$val"}}}],
                        [{_id: null, o: null}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 1}}, {$group: {_id: null, o: {$max: "$myMeta"}}}],
                        [{_id: null, o: 3}],
                        false /* excludeMeta */);
    runGroupRewriteTest(
        coll,
        docs,
        [{$project: {myMeta: 1}}, {$group: {_id: {x: "$myMeta"}, o: {$max: "$myMeta"}}}],
        [{_id: {x: 1}, o: 1}, {_id: {x: 3}, o: 3}],
        false /* excludeMeta */);
    runGroupRewriteTest(
        coll,
        docs,
        [{$project: {myMeta: 1}}, {$group: {_id: {x: "$myMeta"}, o: {$max: "$val"}}}],
        [{_id: {x: 1}, o: null}, {_id: {x: 3}, o: null}],
        false /* excludeMeta */);
    runGroupRewriteTest(
        coll,
        docs,
        [{$project: {myMeta: 1}}, {$group: {_id: {meta: "$myMeta"}, c: {$count: {}}}}],
        [{_id: {meta: 1}, c: 2}, {_id: {meta: 3}, c: 1}],
        false /* excludeMeta */);

    //
    // Tests with exclusion projections.
    //
    runGroupRewriteTest(coll,
                        docs,
                        [
                            {$project: {myMeta: 0}},
                            {$addFields: {m: "$myMeta"}},
                            {$group: {_id: null, o: {$max: "$m"}}}
                        ],
                        [{_id: null, o: null}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 0}}, {$group: {_id: "$myMeta", o: {$max: "$val"}}}],
                        [{_id: null, o: 5}],
                        false /* excludeMeta */);
    runGroupRewriteTest(
        coll,
        docs,
        [{$project: {myMeta: 0}}, {$group: {_id: {x: "$myMeta"}, o: {$max: "$val"}}}],
        [{_id: {x: null}, o: 5}],
        false /* excludeMeta */);
    runGroupRewriteTest(
        coll,
        docs,
        [{$project: {myMeta: 0, val: 0}}, {$group: {_id: {x: "$myMeta"}, o: {$max: "$val"}}}],
        [{_id: {x: null}, o: null}],
        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 0}}, {$group: {_id: null, o: {$max: "$val"}}}],
                        [{_id: null, o: 5}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {myMeta: 0}}, {$group: {_id: null, c: {$count: {}}}}],
                        [{_id: null, c: 3}],
                        false /* excludeMeta */);
})();

// Validates the $addFields/$project(with computed fields) followed by $group scenarios, where the
// projection may project out/modify the fields being used by the $group stage.
(function testMetaGroupKey_WithComputedMetaPrefix() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, val: 3},
        {time: t, myMeta: 3, val: 4},
        {time: t, myMeta: 1, val: 5},
    ];
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {time: "$myMeta"}}, {$group: {_id: null, o: {$max: "$time"}}}],
                        [{_id: null, o: 3}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$project: {val: "$myMeta"}}, {$group: {_id: null, o: {$max: "$val"}}}],
                        [{_id: null, o: 3}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [{$addFields: {myMeta: "5"}}, {$group: {_id: null, o: {$max: "$myMeta"}}}],
                        [{_id: null, o: "5"}],
                        false /* excludeMeta */);

    runGroupRewriteTest(
        coll,
        docs,
        [{$addFields: {myMeta: "x"}}, {$group: {_id: "$myMeta", o: {$max: "$val"}}}],
        [{_id: "x", o: 5}],
        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [
                            {$addFields: {myMeta: "y"}},
                            {$project: {val: "$myMeta"}},
                            {$group: {_id: {x: "$myMeta"}, o: {$max: "$val"}}}
                        ],
                        [{_id: {x: null}, o: "y"}],
                        false /* excludeMeta */);
    runGroupRewriteTest(coll,
                        docs,
                        [
                            {$addFields: {myMeta: "$val"}},
                            {$project: {val: 0}},
                            {$group: {_id: {x: "$myMeta"}, o: {$max: "$val"}}}
                        ],
                        [{_id: {x: 3}, o: null}, {_id: {x: 4}, o: null}, {_id: {x: 5}, o: null}],
                        false /* excludeMeta */);
})();
