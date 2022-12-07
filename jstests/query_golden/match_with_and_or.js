/**
 * Test $match with $and/$or is supported and returns correct results.
 */

(function() {
"use strict";
load("jstests/query_golden/libs/utils.js");

const coll = db.and_or_coll;

const docs = [
    // No 'a' path.
    {x: 2},

    // No 'a.b' path.
    {a: 1, x: 2},
    {a: {c: 1}},
    {a: [1], x: 1},
    {a: [1, 2, 3, "1"]},

    // No 'a.b.c' path.
    {a: {b: 1, x: 1}, x: 2},
    {a: {b: [1, 2], x: 2}, x: 3},
    {a: {b: [2, 3], x: 3}, x: 4},
    {a: [{b: 1, x: 1}, {x: 2}], x: 3},
    {a: [1, 2, {b: 1}, {b: 2}], x: 1},
    {a: [{b: [1, 2], x: 1}, {x: 2}], x: 3},
    {a: [{b: {d: [1, 2, 3]}, x: 1}, {x: 2}], x: 3},

    // 'a.b.c' path exists.
    {a: {b: {c: 1, x: 1}, x: 1}, x: 1},
    {a: [1, 2, {b: [2, {c: 1}, {c: 2}, {x: 1}]}], x: 2},
    {a: [1, 2, {b: 1}, {b: {c: 1}}, {b: {c: 2}}, {x: 1}, {x: {y: 1}}], x: 2},
    {a: [{b: [{c: [1, 2]}]}]},

    // There is a double array under 'a' which should not be traversed.
    {a: [[1, {b: [1, 2, {c: 1}, {c: 2}]}]]},
];
const indexes = [
    {a: 1},
    {'a.b': 1},
    {a: 1, x: 1},
];
resetCollection(coll, docs, indexes);

const andOrSpecs = [
    // Where there is only one child.
    [{a: 1}],
    [{a: {$lt: 20}}],
    [{a: {$lt: 20, $gt: 0}}],

    // Where the predicates are over the same path.
    [{a: 1}, {a: 2}],
    [{a: 1}, {a: 1}],
    [{a: {$in: [1, 3]}}, {a: {$in: [2, 4]}}],
    [{'a.b': 1}, {'a.b': 2}],
    [{a: {$lt: 20}}, {a: {$gt: 0}}],
    [{a: {$gt: 0}}, {a: {$gte: 0}}],
    [{a: {$lte: 1}}, {a: {$gte: 2}}],
    [{a: {$lt: 20, $gt: 0}}, {a: {$lt: 10, $gt: -5}}],
    [{a: {$gt: 1}}, {a: {$lt: "2"}}],
    [{a: null}, {a: {$exists: false}}],
    [{a: {$in: [1, 2, 3]}}, {a: {$in: [3, 4, 5]}}],

    // Where the predicates form a contradiction.
    [{a: 1}, {a: {$not: {$eq: 1}}}],
    [{a: {$exists: false}}, {a: {$exists: true}}],
    [{'a.b': {$exists: false}}, {'a.b': {$exists: true}}],

    // Where the predicates are over paths with shared prefixes.
    [{'a.b': 1}, {'a.x': 1}],
    [{'a.b.c': 1}, {'a.x.y': 1}],
    [{'a.b.c': 1}, {'a.b.x': 1}],
    [{'a.b': 1}, {'a.b.c': 2}],

    // Where some of the predicates are over completely disjoint paths.
    [{a: 1}, {x: 2}],
    [{a: 1}, {a: 2}, {'a.b': 1}, {x: 2}],

    // Where one of the predicates is always-false or always-true.
    [{_id: {$exists: true}}, {a: 1}],
    [{/* should match everything */}, {a: 1}],
    [{_id: {$exists: false}}, {a: 1}],

    // Where the predicates are themselves ands/ors: 1 level of nesting.
    [{$or: [{'a.b': 1}, {'a.b': 2}]}, {$and: [{x: {$exists: true}}, {a: {$exists: true}}]}],

    // Where the predicates are themselves ands/ors: 2 levels of nesting.
    [
        {
            $or: [
                {$or: [{'a.b': 1}, {'a.b': 2}]},
                {$and: [{x: {$exists: true}}, {'a.b.c': {$exists: true}}]}
            ]
        },
        {$and: [{$or: [{'a.b.c': 1}, {'a.b.c': 2}]}, {$and: [{a: 1}, {a: 2}]}]}
    ],
];

const operators = ["$and", "$or"];
for (const op of operators) {
    for (const andOrSpec of andOrSpecs) {
        const pipeline = [{$match: {[op]: andOrSpec}}];
        jsTestLog(`Query: ${tojsononeline(pipeline)}`);
        show(coll.aggregate(pipeline));
    }
}
}());
