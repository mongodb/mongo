/**
 * Tests that the optimization for $sort followed by $group with $first/$last works for timeseries
 * collections:
 *
 * The top-k sort optimization absorbs a $sort stage that is enough to produce a top-k sorted input
 * for a group key if the $sort is followed by $group with $first and/or $last.
 *
 * For example, the following pipeline can be rewritten into a $group with $top/$bottom:
 * [
 *   {$_internalUnpackBucket: {...}},
 *   {$sort: {b: 1}},
 *   {$group: {_id: "$a", f: {$first: "$b"}, l: {$last: "$b"}}
 * ]
 *
 * The rewritten pipeline would be:
 * [
 *   {$_internalUnpackBucket: {...}},
 *   {
 *     $group: {
 *       _id: "$a",
 *       f: {$top: {sortBy: {b: 1}, output: "$b"}},
 *       l: {$bottom: {sortBy: {b: 1}, output: "$b"}}
 *     }
 *   }
 * ]
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   tenant_migration_incompatible,
 *   # Verifying the optimization is applied through explain works in FCV 8.0 and forward.
 *   requires_fcv_80,
 *   # setParameter is not allowed with signed security.
 *   not_allowed_with_signed_security_token,
 *   # $accumulator requires server-side scripting
 *   requires_scripting,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getCallerName} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getSingleNodeExplain
} from "jstests/libs/query/analyze_plan.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

// 't' is the time field, 's' is the meta field, 'x' and 'y' are the measurement fields.
// 't1' starts at 2024-01-01T23:50:00 and is out of boundary for t2-t9 in hour unit.
const doc_t1_sD_x3_y10 = {
    t: ISODate("2024-01-01T23:50:00"),
    s: "D",
    x: 3,
    y: 10
};
const doc_t1_sD_adotb_0_ = {
    t: ISODate("2024-01-01T23:50:00"),
    s: "D",
    a: [{b: 0}],
};
const doc_t2_sA_x10_y5 = {
    t: ISODate("2024-01-02T00:00:28Z"),
    s: "A",
    x: 10,
    y: 5
};
const doc_t2_sA_adotb_1_99_ = {
    t: ISODate("2024-01-02T00:00:28Z"),
    s: "A",
    a: [{b: 1}, {b: 99}],  // extract 1 for {"a.b": 1} sort key and 99 for {"a.b": -1} sort key
};
const doc_t3_sB_x2_y100 = {
    t: ISODate("2024-01-02T00:05:17Z"),
    s: "B",
    x: 2,
    y: 100
};
const doc_t3_sB_adotb2 = {
    t: ISODate("2024-01-02T00:05:17Z"),
    s: "B",
    a: {b: 2},
};
const doc_t4_sB_x5_y24 = {
    t: ISODate("2024-01-02T00:10:42Z"),
    s: "B",
    x: 5,
    y: 24
};
const doc_t5_sA_x4_y1 = {
    t: ISODate("2024-01-02T00:15:50Z"),
    s: "A",
    x: 4,
    y: 1
};
const doc_t5_sA_adotb_50_98_ = {
    t: ISODate("2024-01-02T00:15:50Z"),
    s: "A",
    a: [{b: 50}, {b: 98}],  // extract 50 for {"a.b": 1} sort key and 98 for {"a.b": -1} sort key
};
const doc_t6_sC_x5_y9 = {
    t: ISODate("2024-01-02T00:20:06Z"),
    s: "C",
    x: 5,
    y: 9
};
const doc_t6_sC_adotb_100_ = {
    t: ISODate("2024-01-02T00:20:06Z"),
    s: "C",
    a: [{b: 100}]
};
const doc_t7_sC_x7_y300 = {
    t: ISODate("2024-01-02T00:21:06Z"),
    s: "C",
    x: 7,
    y: 300
};
// Nothing at the path "$a.b"
const doc_t7_sC_a_50_51_ = {
    t: ISODate("2024-01-02T00:21:06Z"),
    s: "C",
    a: [50, 51]
};
const doc_t8_sD_x20_y3 = {
    t: ISODate("2024-01-02T00:22:59Z"),
    s: "D",
    x: 20,
    y: 3
};
const doc_t9_sC_x6_y1000 = {
    t: ISODate("2024-01-02T00:23:01"),
    s: "C",
    x: 6,
    y: 1000
};
// 't10' starts at 2024-01-02T01:05:01 and is out of boundary for t2-t9 in hour unit.
const doc_t10_sB_x6_y1 = {
    t: ISODate("2024-01-02T01:05:01"),
    s: "B",
    x: 6,
    y: 1
};

function prepareCollection(collName, docs) {
    testDB[collName].drop();

    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "s"}}));
    const coll = testDB[collName];
    assert.commandWorked(coll.insert(docs));

    return coll;
}

function D(pipeline, explain) {
    return `pipeline = ${tojson(pipeline)}, explain = ${tojson(explain)}`;
}

function verifyOptimizedForClassic(pipeline, explain) {
    const sortStages = getAggPlanStages(explain, "$sort");
    const groupStages = getAggPlanStages(explain, "$group");
    assert.eq(sortStages.length,
              0,
              `W/ the optimization, $sort should not appear in the plan: ${D(pipeline, explain)}`);
    assert.eq(groupStages.length, 1, `Expected a single $group stage: ${D(pipeline, explain)}`);
}

function verifySortNotAbsorbedForClassic(pipeline, explain) {
    const sortStages = getAggPlanStages(explain, "$sort");
    const groupStages = getAggPlanStages(explain, "$group");
    assert.eq(sortStages.length,
              1,
              `W/o the optimization, $sort should appear in the plan: ${D(pipeline, explain)}`);
    assert.eq(groupStages.length, 1, `Expected a single $group stage: ${D(pipeline, explain)}`);
}

function verifyOptimizedForSbe(pipeline, explain) {
    const groupStages = (() => {
        if (explain.hasOwnProperty("stages")) {
            return getPlanStages(explain.stages[0].$cursor, "GROUP");
        } else {
            return getPlanStages(explain, "GROUP");
        }
    })();
    assert.eq(groupStages.length, 1, `Expected a single GROUP stage: ${D(pipeline, explain)}`);
    assert.eq(groupStages[0].inputStage.stage,
              "UNPACK_TS_BUCKET",
              `Expected the GROUP absorbed SORT: ${D(pipeline, explain)}`);
}

function verifySortNotAbsorbedForSbe(pipeline, explain) {
    const sortStages = (() => {
        if (explain.hasOwnProperty("stages")) {
            return getPlanStages(explain.stages[0].$cursor, "SORT");
        } else {
            return getPlanStages(explain, "SORT");
        }
    })();
    if (sortStages.length == 0) {
        // When only UNPACK_TS_BUCKET is lowered to SBE
        const sortAggStages = getAggPlanStages(explain, "$sort");
        assert.eq(sortAggStages.length,
                  1,
                  `W/o the optimization, $sort should appear in the plan: ${D(pipeline, explain)}`);
    } else if (sortStages.length > 0) {
        // When SORT and UNPACK_TS_BUCKET is lowered together to SBE
        assert.eq(sortStages.length, 1, `Expected a single SORT stage: ${D(pipeline, explain)}`);
        assert.eq(sortStages[0].inputStage.stage,
                  "UNPACK_TS_BUCKET",
                  `Expected the SORT not absorbed: ${D(pipeline, explain)}`);
    }
}

function verifyOptimized(pipeline, explain) {
    if (getEngine(explain) === "classic") {
        verifyOptimizedForClassic(pipeline, explain);
    } else {
        verifyOptimizedForSbe(pipeline, explain);
    }
}

function verifyOptimizedByBoundedSort(pipeline, explain) {
    if (getEngine(explain) === "classic") {
        const sortStages = getAggPlanStages(explain, "$_internalBoundedSort");
        assert.eq(
            sortStages.length,
            1,
            `W/o the optimization, $_internalBoundedSort should appear: ${D(pipeline, explain)}`);
    }
}

function verifySortNotAbsorbed(pipeline, explain) {
    if (getEngine(explain) === "classic") {
        verifySortNotAbsorbedForClassic(pipeline, explain);
    } else {
        verifySortNotAbsorbedForSbe(pipeline, explain);
    }
}

function runTestCase({
    setup = (db) => {},
    docs,
    pipeline,
    expected,
    // By default, verifies that the pipeline is optimized in both the classic engine and the SBE.
    verifyThis = (pipeline, explain) => verifyOptimized(pipeline, explain),
    tearDown = (db) => {}
}) {
    jsTestLog(`Running ${getCallerName()}`);
    setup(db);

    try {
        const collName = getCallerName();
        const coll = prepareCollection(collName, docs);
        const explainFull = coll.explain().aggregate(pipeline);
        const explain = getSingleNodeExplain(explainFull);
        jsTestLog(`Explain: ${tojson(explain)}`);
        verifyThis(pipeline, explain);
        const results = coll.aggregate(pipeline).toArray();
        assertArrayEq({expected: expected, actual: results});
    } finally {
        tearDown(db);
    }
}

(function testBasicCase() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t3_sB_x2_y100,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1
        ],
        pipeline:
            [{$sort: {t: 1}}, {$group: {_id: "$s", open: {$first: "$x"}, close: {$last: "$x"}}}],
        expected: [
            {_id: "A", open: 10, close: 4},
            {_id: "B", open: 2, close: 6},
            {_id: "C", open: 5, close: 6},
            {_id: "D", open: 3, close: 20}
        ],
    });
})();

(function testMixedAccs() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t3_sB_x2_y100,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1
        ],
        pipeline: [{$sort: {t: 1}}, {$group: {_id: "$s", open: {$first: "$x"}, s: {$sum: "$y"}}}],
        expected: [
            {_id: "A", open: 10, s: 6},
            {_id: "B", open: 2, s: 125},
            {_id: "C", open: 5, s: 1309},
            {_id: "D", open: 3, s: 13}
        ],
    });
})();

(function testGroupByTime() {
    runTestCase({
        docs: [doc_t1_sD_x3_y10, doc_t2_sA_x10_y5, doc_t3_sB_x2_y100, doc_t10_sB_x6_y1],
        pipeline: [
            {$sort: {t: 1}},
            {
                $group: {
                    _id: {$dateTrunc: {unit: "hour", date: "$t"}},
                    fs: {$first: "$s"},
                    ls: {$last: "$s"},
                }
            }
        ],
        expected: [
            {_id: ISODate("2024-01-01T23:00:00Z"), fs: "D", ls: "D"},
            {_id: ISODate("2024-01-02T00:00:00Z"), fs: "A", ls: "B"},
            {_id: ISODate("2024-01-02T01:00:00Z"), fs: "B", ls: "B"},
        ],
    });
})();

(function testMultipleGroupByKeys() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t8_sD_x20_y3,
            doc_t6_sC_x5_y9,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1
        ],
        pipeline: [
            {$sort: {t: 1}},
            {
                $group: {
                    _id: {t: {$dateTrunc: {unit: "hour", date: "$t"}}, s: "$s"},
                    fy: {$first: "$y"},
                    ly: {$last: "$y"},
                }
            },
        ],
        expected: [
            {_id: {t: ISODate("2024-01-01T23:00:00Z"), s: "D"}, fy: 10, ly: 10},
            {_id: {t: ISODate("2024-01-02T00:00:00Z"), s: "A"}, fy: 5, ly: 5},
            {_id: {t: ISODate("2024-01-02T00:00:00Z"), s: "C"}, fy: 9, ly: 1000},
            {_id: {t: ISODate("2024-01-02T00:00:00Z"), s: "D"}, fy: 3, ly: 3},
            {_id: {t: ISODate("2024-01-02T01:00:00Z"), s: "B"}, fy: 1, ly: 1},
        ],
    });
})();

(function testOptimizedManyFirstsAndLasts() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t3_sB_x2_y100,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1
        ],
        pipeline: [
            {$sort: {t: 1}},
            {
                $group: {
                    _id: "$s",
                    fx: {$first: "$x"},
                    lx: {$last: "$x"},
                    fy: {$first: "$y"},
                    ly: {$last: "$y"},
                }
            },
        ],
        expected: [
            {_id: "A", fx: 10, lx: 4, fy: 5, ly: 1},
            {_id: "B", fx: 2, lx: 6, fy: 100, ly: 1},
            {_id: "C", fx: 5, lx: 6, fy: 9, ly: 1000},
            {_id: "D", fx: 3, lx: 20, fy: 10, ly: 3},
        ],
    });
})();

(function testSortByArrayAsc() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t5_sA_adotb_50_98_,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [
            {$sort: {"a.b": 1}},
            {
                $group: {
                    _id: "$s",
                    fa: {$first: "$a.b"},
                    la: {$last: "$a.b"},
                }
            },
        ],
        expected: [
            // For _id: "A", we extract 1 from [1, 99] and 50 from [50, 98] for {"a.b": 1} sort key.
            {_id: "A", fa: [1, 99], la: [50, 98]},
            {_id: "B", fa: 2, la: 2},
            {_id: "C", fa: [], la: [100]},
            {_id: "D", fa: [0], la: [0]},
        ],
    });
})();

(function testSortByArrayDesc() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t5_sA_adotb_50_98_,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [
            {$sort: {"a.b": -1}},
            {
                $group: {
                    _id: "$s",
                    fa: {$first: "$a.b"},
                    la: {$last: "$a.b"},
                }
            },
        ],
        expected: [
            // For _id: "A", we extract 99 from [1, 99] and 98 from [50, 98] for {"a.b": -1} sort
            // key. Hence, [1, 99] is the first and [50, 98] is the last.
            {_id: "A", fa: [1, 99], la: [50, 98]},
            {_id: "B", fa: 2, la: 2},
            {_id: "C", fa: [100], la: []},
            {_id: "D", fa: [0], la: [0]},
        ],
    });
})();

(function testNotOptimizedOtherStageBetweenSortAndGroup() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [
            {$sort: {"a.b": 1}},
            {$addFields: {arrsum: {$sum: "$a.b"}}},
            {
                $group: {
                    _id: "$s",
                    fa: {$first: "$arrsum"},
                    la: {$last: "$arrsum"},
                }
            },
        ],
        expected: [
            {_id: "A", fa: 100, la: 100},
            {_id: "B", fa: 2, la: 2},
            {_id: "C", fa: 0, la: 100},
            {_id: "D", fa: 0, la: 0},
        ],
        verifyThis: (pipeline, explain) => verifySortNotAbsorbed(pipeline, explain)
    });
})();

(function testNotOptimizedSortLimit() {
    runTestCase({
        docs: [
            doc_t4_sB_x5_y24,  // The last time among input docs comes first.
            doc_t1_sD_x3_y10,
            doc_t3_sB_x2_y100,
            doc_t2_sA_x10_y5,
        ],
        pipeline: [{$sort: {t: 1}}, {$limit: 3}, {$group: {_id: "$s", open: {$first: "$x"}}}],
        expected: [{_id: "D", open: 3}, {_id: "A", open: 10}, {_id: "B", open: 2}],
        verifyThis: (pipeline, explain) => verifyOptimizedByBoundedSort(pipeline, explain)
    });
})();

(function testMixedFirstAndFirstN() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t3_sB_x2_y100,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1,
        ],
        pipeline: [
            {$sort: {t: 1}},
            {$group: {_id: "$s", f1: {$first: "$x"}, f2: {$firstN: {n: 2, input: "$y"}}}}
        ],
        expected: [
            {_id: "A", f1: 10, f2: [5, 1]},
            {_id: "B", f1: 2, f2: [100, 24]},
            {_id: "C", f1: 5, f2: [9, 300]},
            {_id: "D", f1: 3, f2: [10, 3]}
        ],
        verifyThis: (pipeline, explain) => verifyOptimizedByBoundedSort(pipeline, explain)
    });
})();

(function testNotOptimizedSortByMetaAndGroup() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t3_sB_x2_y100,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1,
        ],
        pipeline: [
            {$sort: {x: {$meta: "randVal"}}},
            {$group: {_id: "$s", open: {$first: "$x"}, close: {$last: "$x"}}}
        ],
        expected: [
            {_id: "A", open: 10, close: 4},
            {_id: "B", open: 2, close: 6},
            {_id: "C", open: 5, close: 6},
            {_id: "D", open: 3, close: 20}
        ],
        verifyThis: (pipeline, explain) => {
            const sortStages = getAggPlanStages(explain, "$sort");
            assert.eq(sortStages.length,
                      1,
                      `W/o the optimization, $sort should appear: ${D(pipeline, explain)}`);
        }
    });
})();

const doc_t1_sD_x3_adotb0 = {
    t: ISODate("2024-01-01T23:50:00"),
    s: "D",
    x: 3,
    a: {b: 0}
};

const doc_t2_sA_x2_adotc1 = {
    t: ISODate("2024-01-01T23:51:00"),
    s: "A",
    x: 2,
    a: {c: 1}
};

const doc_t3_sD_x1_adotb1 = {
    t: ISODate("2024-01-02T00:02:00"),
    s: "D",
    x: 1,
    a: {b: 1}
};

(function testNotOptimizedMixedFirstAndMergeObjects() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_adotb0,
            doc_t2_sA_x2_adotc1,
            doc_t3_sD_x1_adotb1,
        ],
        pipeline: [
            {$sort: {x: 1}},
            {
                $group: {
                    _id: "$s",
                    fx: {$first: "$x"},
                    o: {$mergeObjects: "$a"},
                }
            },
        ],
        // For {s: "D"}, {$sort: {x: 1}} will return 'doc_t3_sD_x1_adotb1' first and then
        // 'doc_t1_sD_x3_adotb0' which will overwrite any duplicate fields. Hence, 'o' value will
        // be {b: 0}.
        expected: [
            {_id: "A", fx: 2, o: {c: 1}},
            {_id: "D", fx: 1, o: {b: 0}},
        ],
        verifyThis: (pipeline, explain) => verifySortNotAbsorbed(pipeline, explain)
    });
})();

(function testNotOptimizedMixedLastAndPush() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_adotb0,
            doc_t2_sA_x2_adotc1,
            doc_t3_sD_x1_adotb1,
        ],
        pipeline: [
            {$sort: {x: 1}},
            {
                $group: {
                    _id: "$s",
                    lx: {$last: "$x"},
                    a: {$push: "$a"},
                }
            },
        ],
        // For {s: "D"}, {$sort: {x: 1}} will return 'doc_t3_sD_x1_adotb1' first and then
        // 'doc_t1_sD_x3_adotb0'. Hence, 'a' value will be [{b: 1}, {b: 0}].
        expected: [
            {_id: "A", lx: 2, a: [{c: 1}]},
            {_id: "D", lx: 3, a: [{b: 1}, {b: 0}]},
        ],
        verifyThis: (pipeline, explain) => verifySortNotAbsorbed(pipeline, explain)
    });
})();

(function testNotOptimizedMixedLastAndAccumulatorJs() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_adotb0,
            doc_t2_sA_x2_adotc1,
            doc_t3_sD_x1_adotb1,
        ],
        pipeline: [
            {$sort: {x: 1}},
            {
                $group: {
                    _id: "$s",
                    lx: {$last: "$x"},
                    // This simulates $push.
                    a: {
                        $accumulator: {
                            init: function() {
                                return [];
                            },
                            accumulate: function(state, fieldA) {
                                state.push(fieldA);
                                return state;
                            },
                            accumulateArgs: ["$a"],
                            merge: function(state1, state2) {
                                return state1.concat(state2);
                            },
                            lang: "js"
                        }
                    },
                }
            },
        ],
        // For {s: "D"}, {$sort: {x: 1}} will return 'doc_t3_sD_x1_adotb1' first and then
        // 'doc_t1_sD_x3_adotb0'. Hence, 'a' value will be [{b: 1}, {b: 0}].
        expected: [
            {_id: "A", lx: 2, a: [{c: 1}]},
            {_id: "D", lx: 3, a: [{b: 1}, {b: 0}]},
        ],
        verifyThis: (pipeline, explain) => verifySortNotAbsorbed(pipeline, explain)
    });
})();
