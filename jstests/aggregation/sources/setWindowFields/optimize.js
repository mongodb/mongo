/**
 * Test that redundant sorts are removed/swapped.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 *   requires_fcv_50,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');

// Find how many stages of the plan are 'stageName'.
function numberOfStages(explain, stageName) {
    return getAggPlanStages(explain, stageName).length;
}

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert([
    {a: {b: 3, c: 3}, b: 2, c: 1},
    {a: {b: 3, c: 3}, b: 4, c: 1},
    {a: {b: 1, c: 1}, b: 4, c: 1},
    {a: {b: 2, c: 2}, b: 1, c: 1},
    {a: {b: 1, c: 1}, b: 4, c: 1},
    {a: {b: 3, c: 3}, b: 2, c: 1},
    {a: {b: 2, c: 2}, b: 1, c: 1},
    {a: {b: 2, c: 2}, b: 1, c: 1},
    {a: {b: 2, c: 2}, b: 3, c: 1},
    {a: {b: 1, c: 1}, b: 4, c: 1},
]));

const explain1 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1}}
]);

// Redundant $sort should be removed.
assert.eq(1, numberOfStages(explain1, '$sort'), explain1);
// We keep the more specific sort.
assert.docEq(getAggPlanStages(explain1, '$sort'), [{$sort: {sortKey: {a: 1, b: 1}}}], explain1);

const explain2 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: -1}}
]);

// $sort is not redundant, should not be removed.
assert.eq(2, numberOfStages(explain2, '$sort'), explain2);

const explain3 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1, b: -1}}
]);

// $sort should be swapped with $_internalSetWindowFields, and the extra one removed.
assert.eq(1, numberOfStages(explain3, '$sort'), explain3);
// The sort we keep should be the more specific one.
assert.docEq(getAggPlanStages(explain3, '$sort'), [{$sort: {sortKey: {a: 1, b: -1}}}], explain3);

const explain4 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {a: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1}}
]);

// Sort field is modified, can't be swapped or removed.
assert.eq(2, numberOfStages(explain4, '$sort'), explain4);

const explain5 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {'a.b': {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1}}
]);

// Sort field is modified, can't be swapped or removed.
assert.eq(2, numberOfStages(explain5, '$sort'), explain5);

const explain6 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1}},
    {$limit: 5}
]);

// $sort + $limit should not be merged.
assert(aggPlanHasStage(explain6, "$limit"), explain6);
// The $limit should not prevent us from removing the redundant $sort.
assert.eq(1, numberOfStages(explain6, '$sort'), explain6);

const explain7 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {$setWindowFields: {partitionBy: "$a.b", output: {sum: {$sum: "$c"}}}},
    {$sort: {'a.b': 1}}
]);

// Sort should be removed if sorting and partitioning on same field.
assert.eq(1, numberOfStages(explain7, '$sort'), explain7);

const explain8 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            partitionBy: "$a.c",
            sortBy: {'a.b': 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {'a.b': 1}}
]);

// Sort should not be removed since primary sort field is "a.c".
assert.eq(2, numberOfStages(explain8, '$sort'), explain8);

const explain9 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            partitionBy: "$a.c",
            sortBy: {'a.b': 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {'a.c': 1}}
]);

// Sort should be removed since primary sort field is "a.c".
assert.eq(1, numberOfStages(explain9, '$sort'), explain9);

// Multiple redundant sorts are dropped.
const explain10 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1, b: 1, c: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1, b: 1}},
    {$sort: {a: 1}},
    {$sort: {a: 1, b: 1}},
]);
assert.eq(1, numberOfStages(explain10, '$sort'), explain10);
assert.docEq(
    getAggPlanStages(explain10, '$sort'), [{$sort: {sortKey: {a: 1, b: 1, c: 1}}}], explain10);

// Multiple compatible sorts are pushed down.
const explain11 = coll.explain().aggregate([
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: 1, b: 1, c: 1}},
    {$sort: {a: 1, b: 1}},
    {$sort: {a: 1, b: 1, c: 1}},
]);
assert.eq(1, numberOfStages(explain11, '$sort'), explain11);
assert.docEq(
    getAggPlanStages(explain11, '$sort'), [{$sort: {sortKey: {a: 1, b: 1, c: 1}}}], explain11);

// An incompatible $meta sort should not be dropped or pushed down.
coll.createIndex({'$**': 'text'});
const explain12 = coll.explain().aggregate([
    {$match: {$text: {$search: 'hi'}}},
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: 1},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: {$meta: "textScore"}}},
]);
assert.eq(2, numberOfStages(explain12, '$sort'), explain12);

// For now, we don't handle $meta at all: it won't be optimized even if it's compatible.
const explain13 = coll.explain().aggregate([
    {$match: {$text: {$search: 'hi'}}},
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {a: {$meta: "textScore"}},
            output: {sum: {$sum: "$c", window: {documents: ["unbounded", "current"]}}}
        }
    },
    {$sort: {a: {$meta: "textScore"}}},
]);
assert.eq(2, numberOfStages(explain13, '$sort'), explain13);
})();
