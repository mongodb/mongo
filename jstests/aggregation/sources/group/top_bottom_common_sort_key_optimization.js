/**
 * Tests that multiple top(N)/bottom(N) with the same sort pattern and the same N expression are
 * optimized into one top(N) or bottom(N) so that they share the same sort key.
 *
 * @tags: [
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # The top/bottom common sort key optimization is available since FCV 8.0.
 *   requires_fcv_80,
 *   requires_pipeline_optimization,
 *   # We don't want to verify that the optimization is applied inside $facet since its shape is
 *   # quite different from the original one.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getCallerName} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getSingleNodeExplain
} from "jstests/libs/analyze_plan.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

function prepareCollection(collName, docs) {
    testDB[collName].drop();

    const coll = testDB[collName];
    assert.commandWorked(coll.insert(docs));

    return coll;
}

function D(pipeline, explain) {
    return `pipeline = ${tojson(pipeline)}, explain = ${tojson(explain)}`;
}

// The following functions verify optimized pipeline.
//
// These functions verify the optimized pipeline by a rough plan shape instead of the exact plan
// shape because the applicable optimizations may evolve as time passes.
function verifyOptimizedForClassic(pipeline, explain) {
    const groupStages = getAggPlanStages(explain, "$group");
    const projectStages = getAggPlanStages(explain, "$project");
    assert.eq(groupStages.length, 1, `Expected a single $group stage: ${D(pipeline, explain)}`);
    assert.eq(projectStages.length,
              1,
              `W/ the optimization, $project should appear in the plan: ${D(pipeline, explain)}`);
}

function verifyOptimizedForSbe(pipeline, explain) {
    const groupStages = (() => {
        if (explain.hasOwnProperty("stages")) {
            return getPlanStages(explain.stages[0].$cursor, "GROUP");
        } else {
            return getPlanStages(explain, "GROUP");
        }
    })();
    const projectStages = (() => {
        if (explain.hasOwnProperty("stages")) {
            return getAggPlanStages(explain, "$project");
        } else {
            return getPlanStages(explain, "PROJECTION_DEFAULT");
        }
    })();
    assert.eq(groupStages.length, 1, `Expected a single GROUP stage: ${D(pipeline, explain)}`);
    assert.eq(projectStages.length,
              1,
              `W/ the optimization, $project or PROJECTION_DEFAULT should appear in the plan: ${
                  D(pipeline, explain)}`);
}

function verifyOptimizedForMerger(pipeline, mergerPart) {
    // The merger part will be in a shape of $mergeCursors - $group - $project if the optimization
    // is applied.
    let groupFound = false;
    let projectFound = false;
    for (const stage of mergerPart) {
        if ("$group" in stage) {
            groupFound = true;
        } else if ("$project" in stage) {
            projectFound = true;
        }
    }

    assert(groupFound && projectFound,
           `Both $group and $project should exist in the merger part: ${D(pipeline, mergerPart)}`);
}

function verifyOptimized(pipeline, explainFull) {
    let isShardedCollection = false;
    const explain = (() => {
        // If the collection is sharded, the pipeline is split at the $group stage and the shard
        // plan part does not have $project which is added as part of the optimization. So, we
        // verify only that the the merger part plan has both $group & $project stages.
        if (explainFull.hasOwnProperty("splitPipeline") && explainFull.splitPipeline) {
            isShardedCollection = true;
            return explainFull.splitPipeline.mergerPart;
        } else {
            return getSingleNodeExplain(explainFull);
        }
    })();

    if (isShardedCollection) {
        verifyOptimizedForMerger(pipeline, explain);
        return;
    }

    if (getEngine(explain) === "classic") {
        verifyOptimizedForClassic(pipeline, explain);
    } else {
        verifyOptimizedForSbe(pipeline, explain);
    }
}

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
const doc_t6_sC_x10_adotb_100_ = {
    t: ISODate("2024-01-02T00:20:06Z"),
    s: "C",
    x: 10,
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
        verifyThis(pipeline, explainFull);
        const results = coll.aggregate(pipeline).toArray();
        assertArrayEq({expected: expected, actual: results});
    } finally {
        tearDown(db);
    }
}

(function testMultipleTopsWithSameSortKeyOptimizedIntoOneTop() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t4_sB_x5_y24,
            doc_t9_sC_x6_y1000,  // This has the largest 'y'
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
        ],
        pipeline: [{
            $group: {
                _id: null,
                ts: {$top: {output: "$s", sortBy: {y: -1}}},
                tx: {$top: {output: "$x", sortBy: {y: -1}}},
                // Missing field should be returned as null.
                tz: {$top: {output: "$z", sortBy: {y: -1}}}
            }
        }],
        expected: [{_id: null, ts: "C", tx: 6, tz: null}]
    });
})();

(function testMultipleTopsWithSameSortKeyOptimizedIntoOneTopPerSortKey() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t4_sB_x5_y24,
            doc_t9_sC_x6_y1000,  // This has the largest 'y'
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,
        ],
        pipeline: [{
            $group: {
                _id: null,
                ts: {$top: {output: "$s", sortBy: {y: -1}}},
                tx: {$top: {output: "$x", sortBy: {y: -1}}},
                // Missing field should be returned as null.
                tz: {$top: {output: "$z", sortBy: {y: -1}}},
                ts2: {$top: {output: "$s", sortBy: {x: -1}}},
                tx2: {$top: {output: "$x", sortBy: {x: -1}}},
                // Missing field should be returned as null.
                tz2: {$top: {output: "$z", sortBy: {x: -1}}},
            }
        }],
        expected: [{_id: null, ts: "C", tx: 6, tz: null, ts2: "D", tx2: 20, tz2: null}]
    });
})();

(function testMultipleTopNsWithSameSortKeyOptimizedIntoOneTopN() {
    runTestCase({
        docs: [
            doc_t6_sC_x5_y9,  // Top 3
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t9_sC_x6_y1000,  // Top 0
            doc_t5_sA_x4_y1,
            doc_t4_sB_x5_y24,
            doc_t8_sD_x20_y3,   // Top 1
            doc_t7_sC_x7_y300,  // Top 2
        ],
        pipeline: [{
            $group: {
                _id: null,
                tns: {$topN: {n: 4, output: "$s", sortBy: {t: -1}}},
                tny: {$topN: {n: 4, output: "$y", sortBy: {t: -1}}}
            }
        }],
        expected: [{_id: null, tns: ["C", "C", "D", "C"], tny: [9, 300, 3, 1000]}]
    });
})();

(function testMultipleTopNsWithSameSortKeyOptimizedIntoOneTopNPerSortKey() {
    runTestCase({
        docs: [
            doc_t6_sC_x5_y9,     // Top 3
            doc_t1_sD_x3_y10,    // Bottom 0
            doc_t2_sA_x10_y5,    // Bottom 1
            doc_t9_sC_x6_y1000,  // Top 0
            doc_t5_sA_x4_y1,     // Bottom 3
            doc_t4_sB_x5_y24,    // Bottom 2
            doc_t8_sD_x20_y3,    // Top 1
            doc_t7_sC_x7_y300,   // Top 2
        ],
        pipeline: [{
            $group: {
                _id: null,
                tns: {$topN: {n: 4, output: "$s", sortBy: {t: -1}}},
                tny: {$topN: {n: 4, output: "$y", sortBy: {t: -1}}},
                tns2: {$topN: {n: 4, output: "$s", sortBy: {t: 1}}},
                tny2: {$topN: {n: 4, output: "$y", sortBy: {t: 1}}}
            }
        }],
        expected: [{
            _id: null,
            tns: ["C", "D", "C", "C"],
            tny: [1000, 3, 300, 9],
            tns2: ["D", "A", "B", "A"],
            tny2: [10, 5, 24, 1]
        }]
    });
})();

(function testMultipleBottomsWithSameSortKeyOptimizedIntoOneBottom() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,  // Bottom 0
            doc_t10_sB_x6_y1,
        ],
        pipeline: [{
            $group: {
                _id: null,
                bs: {$bottom: {output: "$s", sortBy: {x: 1}}},
                by: {$bottom: {output: "$y", sortBy: {x: 1}}}
            }
        }],
        expected: [{_id: null, bs: "D", by: 3}]
    });
})();

(function testMultipleBottomsWithSameSortKeyOptimizedIntoOneBottomPerSortKey() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,  // Bottom 0 by 'y'
            doc_t8_sD_x20_y3,   // Bottom 0 by 'x'
            doc_t10_sB_x6_y1,
        ],
        pipeline: [{
            $group: {
                _id: null,
                bs: {$bottom: {output: "$s", sortBy: {x: 1}}},
                by: {$bottom: {output: "$y", sortBy: {x: 1}}},
                bs2: {$bottom: {output: "$s", sortBy: {y: 1}}},
                by2: {$bottom: {output: "$y", sortBy: {y: 1}}},
            }
        }],
        expected: [{_id: null, bs: "D", by: 3, bs2: "C", by2: 300}]
    });
})();

(function testMultipleBottomNsWithSameSortKeyOptimizedIntoOneBottom() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,  // Bottom 2
            doc_t4_sB_x5_y24,
            doc_t5_sA_x4_y1,  // Bottom 0
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t8_sD_x20_y3,  // Bottom 1
            doc_t9_sC_x6_y1000,
        ],
        pipeline: [{
            $group: {
                _id: null,
                bns: {$bottomN: {n: 3, output: "$s", sortBy: {y: -1}}},
                bnx: {$bottomN: {n: 3, output: "$x", sortBy: {y: -1}}}
            }
        }],
        expected: [{_id: null, bns: ["A", "D", "A"], bnx: [10, 20, 4]}]
    });
})();

(function testMultipleBottomNsWithSameSortKeyOptimizedIntoOneBottomPerSortKeyAndN() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,  // Bottom 2
            doc_t4_sB_x5_y24,  // Top 2
            doc_t5_sA_x4_y1,   // Bottom 0
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,   // Top 1
            doc_t8_sD_x20_y3,    // Bottom 1
            doc_t9_sC_x6_y1000,  // Top 0
        ],
        pipeline: [{
            $group: {
                _id: null,
                bns: {$bottomN: {n: 3, output: "$s", sortBy: {y: -1}}},
                bnx: {$bottomN: {n: 3, output: "$x", sortBy: {y: -1}}},
                bns2: {$bottomN: {n: 3, output: "$s", sortBy: {y: 1}}},
                bnx2: {$bottomN: {n: 3, output: "$x", sortBy: {y: 1}}},
                bns3: {$bottomN: {n: 1, output: "$s", sortBy: {y: -1}}},
                bnx3: {$bottomN: {n: 1, output: "$x", sortBy: {y: -1}}}
            }
        }],
        expected: [{
            _id: null,
            bns: ["A", "D", "A"],
            bnx: [10, 20, 4],
            bns2: ["B", "C", "C"],
            bnx2: [5, 6, 7],
            bns3: ["A"],
            bnx3: [4]
        }]
    });
})();

(function testMultipleBottomNsWithSameSortKeySameNOptimizedIntoOneBottomN() {
    runTestCase({
        docs: [
            doc_t5_sA_x4_y1,
            doc_t1_sD_x3_y10,
            doc_t2_sA_x10_y5,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t4_sB_x5_y24,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1,
            doc_t8_sD_x20_y3,
        ],
        pipeline: [{
            $group: {
                _id: {s: "$s"},
                bnx: {
                    $bottomN: {
                        n: {$cond: {if: {$eq: ["$s", "A"]}, then: 1, else: 2}},
                        output: "$x",
                        sortBy: {t: 1}
                    }
                },
                bny: {
                    $bottomN: {
                        n: {$cond: {if: {$eq: ["$s", "A"]}, then: 1, else: 2}},
                        output: "$y",
                        sortBy: {t: 1}
                    }
                }
            }
        }],
        expected: [
            {"_id": {"s": "A"}, "bnx": [4], "bny": [1]},
            {"_id": {"s": "B"}, "bnx": [5, 6], "bny": [24, 1]},
            {"_id": {"s": "C"}, "bnx": [7, 6], "bny": [300, 1000]},
            {"_id": {"s": "D"}, "bnx": [3, 20], "bny": [10, 3]}
        ]
    });
})();

(function testOptimizableTopNsMixedWithIneligibleAndNotOptimizableAccumulators() {
    runTestCase({
        docs: [
            doc_t5_sA_x4_y1,
            doc_t2_sA_x10_y5,
            doc_t6_sC_x5_y9,
            doc_t7_sC_x7_y300,
            doc_t1_sD_x3_y10,  // The first point
            doc_t4_sB_x5_y24,
            doc_t9_sC_x6_y1000,
            doc_t10_sB_x6_y1,
            doc_t8_sD_x20_y3,
        ],
        pipeline: [{
            $group: {
                _id: null,
                bs: {$bottom: {output: "$s", sortBy: {t: -1}}},
                tx: {$top: {output: "$x", sortBy: {t: 1}}},
                ty: {$top: {output: "$y", sortBy: {t: 1}}},
                sumx: {$sum: "$x"}
            }
        }],
        expected: [{_id: null, bs: "D", tx: 3, ty: 10, sumx: 66}]
    });
})();

(function testOptimizableTopsByArrayAsc() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t5_sA_adotb_50_98_,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                tt: {$top: {output: "$t", sortBy: {"a.b": 1}}},
                tadotb: {$top: {output: "$a.b", sortBy: {"a.b": 1}}},
                tadotc: {$top: {output: "$a.c", sortBy: {"a.b": 1}}},
            }
        }],
        expected: [
            {_id: "A", tt: ISODate("2024-01-02T00:00:28Z"), tadotb: [1, 99], tadotc: []},
            // For "B", the "a" field's value is not an array and array traversal does not happen
            // and so "a.b" is missing and returned as null.
            {_id: "B", tt: ISODate("2024-01-02T00:05:17Z"), tadotb: 2, tadotc: null},
            {_id: "C", tt: ISODate("2024-01-02T00:21:06Z"), tadotb: [], tadotc: []},
            {_id: "D", tt: ISODate("2024-01-01T23:50:00Z"), tadotb: [0], tadotc: []}
        ]
    });
})();

(function testOptimizableTopsByArrayDesc() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t5_sA_adotb_50_98_,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                badotb: {$bottom: {output: "$a.b", sortBy: {"a.b": -1}}},
                tt: {$top: {output: "$t", sortBy: {"a.b": -1}}},
                tadotb: {$top: {output: "$a.b", sortBy: {"a.b": -1}}},
            }
        }],
        expected: [
            {_id: "A", badotb: [50, 98], tt: ISODate("2024-01-02T00:00:28Z"), tadotb: [1, 99]},
            {_id: "B", badotb: 2, tt: ISODate("2024-01-02T00:05:17Z"), tadotb: 2},
            {_id: "C", badotb: [], tt: ISODate("2024-01-02T00:20:06Z"), tadotb: [100]},
            {_id: "D", badotb: [0], tt: ISODate("2024-01-01T23:50:00Z"), tadotb: [0]}
        ]
    });
})();

(function testOptimizableTopNsAndBottomNsWithArrayFields() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t3_sB_adotb2,
            doc_t5_sA_adotb_50_98_,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_a_50_51_,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                tadotb: {$topN: {n: 1, output: "$a.b", sortBy: {"a.b": 1}}},
                tadotc: {$topN: {n: 1, output: "$a.c", sortBy: {"a.b": 1}}},
                badotb: {$bottomN: {n: 1, output: "$a.b", sortBy: {"a.b": 1}}},
                badotc: {$bottomN: {n: 1, output: "$a.c", sortBy: {"a.b": 1}}},
            }
        }],
        expected: [
            {_id: "A", tadotb: [[1, 99]], tadotc: [[]], badotb: [[50, 98]], badotc: [[]]},
            {_id: "B", tadotb: [2], tadotc: [null], badotb: [2], badotc: [null]},
            {_id: "C", tadotb: [[]], tadotc: [[]], badotb: [[100]], badotc: [[]]},
            {_id: "D", tadotb: [[0]], tadotc: [[]], badotb: [[0]], badotc: [[]]}
        ]
    });
})();

(function testOptimizableTopNsAndBottomNsWithMissingFields() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t1_sD_adotb_0_,
            doc_t2_sA_x10_y5,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                // "z" is a missing field
                t1: {$topN: {n: 1, output: "$z", sortBy: {"s": 1}}},
                t2: {$topN: {n: 1, output: "$z", sortBy: {"s": 1}}},
                b1: {$bottomN: {n: 1, output: "$z", sortBy: {"s": 1}}},
                b2: {$bottomN: {n: 1, output: "$z", sortBy: {"s": 1}}},
            }
        }],
        expected: [
            {_id: "A", t1: [null], t2: [null], b1: [null], b2: [null]},
            {_id: "D", t1: [null], t2: [null], b1: [null], b2: [null]},
        ]
    });
})();

(function testOptimizableTopNsWithHighNsAndMissingFields() {
    runTestCase({
        docs: [
            doc_t1_sD_x3_y10,
            doc_t1_sD_adotb_0_,
            doc_t3_sB_adotb2,
            doc_t2_sA_x10_y5,
            doc_t6_sC_x5_y9,
            doc_t6_sC_adotb_100_,
            doc_t7_sC_x7_y300,
            doc_t9_sC_x6_y1000,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                // "z" is always missing, "y" is sometimes missing
                t1: {$topN: {n: 3, output: "$z", sortBy: {"y": 1}}},
                t2: {$topN: {n: 3, output: "$y", sortBy: {"y": 1}}},
            }
        }],
        expected: [
            {_id: "A", t1: [null], t2: [5]},
            {_id: "B", t1: [null], t2: [null]},
            {_id: "C", t1: [null, null, null], t2: [null, 9, 300]},
            {_id: "D", t1: [null, null], t2: [null, 10]},
        ]
    });
})();

(function testOptimizableTopNsAndBottomNsWithDottedPathsAsMissingFields() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                // "z" and "z.z" are missing fields
                t1: {$topN: {n: 1, output: "$z.z", sortBy: {"z.z": 1}}},
                t2: {$topN: {n: 1, output: "$z.z", sortBy: {"z.z": 1}}},
                // "a" exists but "a.z" is missing
                b1: {$bottomN: {n: 1, output: "$a.z", sortBy: {"a.z": 1}}},
                b2: {$bottomN: {n: 1, output: "$a.z", sortBy: {"a.z": 1}}},
            }
        }],
        expected: [
            {_id: "A", t1: [null], t2: [null], b1: [[]], b2: [[]]},
            {_id: "D", t1: [null], t2: [null], b1: [[]], b2: [[]]},
        ]
    });
})();

(function testOptimizableTopNsAndBottomNsWithHighNsAndDottedPathsAsMissingFields() {
    runTestCase({
        docs: [
            doc_t1_sD_adotb_0_,
            doc_t2_sA_adotb_1_99_,
            doc_t6_sC_x5_y9,
            doc_t6_sC_x10_adotb_100_,
            doc_t7_sC_a_50_51_,
            doc_t7_sC_x7_y300,
            doc_t9_sC_x6_y1000,
        ],
        pipeline: [{
            $group: {
                _id: "$s",
                // "z" and "z.z" are missing fields
                t1: {$topN: {n: 3, output: "$z.z", sortBy: {"z.z": 1}}},
                t2: {$topN: {n: 3, output: "$z.z", sortBy: {"z.z": 1}}},
                // "a" sometimes exists but "a.z" is always missing
                b1: {$bottomN: {n: 3, output: "$a.z", sortBy: {x: 1}}},
                b2: {$bottomN: {n: 3, output: "$a.z", sortBy: {x: 1}}},
            }
        }],
        expected: [
            {_id: "A", t1: [null], t2: [null], b1: [[]], b2: [[]]},
            {
                _id: "C",
                t1: [null, null, null],
                t2: [null, null, null],
                b1: [null, null, []],
                b2: [null, null, []]
            },
            {_id: "D", t1: [null], t2: [null], b1: [[]], b2: [[]]},
        ]
    });
})();
