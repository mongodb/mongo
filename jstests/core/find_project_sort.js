/**
 * Test a variety of predicates for the find command's filter, projection and sort expressions.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq and orderedArrayEq.

const coll = db.find_project_sort;
coll.drop();
const documents = [
    {_id: 0, a: 1, b: "x", c: 10},
    {_id: 1, a: 2, b: "y", c: 11},
    {_id: 2, a: 3, b: "z", c: 12},
    {_id: 3, x: {y: 1}},
    {_id: 4, x: {y: 2}},
    {_id: 5, x: {y: [1, 2, 3]}, v: {w: [4, 5, 6]}},
    {_id: 6, x: {y: 4}, v: {w: 4}},
    {_id: 7, x: [{y: 1}], v: [{w: 1}]},
    {_id: 8, x: [{y: 1}, {y: 2}], v: [{w: 5}, {w: 6}]},
    {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{w: 4}, {w: [4, 5, 6]}]},
    {_id: 10, z: 1},
    {_id: 11, z: 2},
    {_id: 12, z: [1, 2, 3]},
    {_id: 13, z: 3},
    {_id: 14, z: 4},
    {_id: 15, a: 10, x: 1},
    {_id: 16, a: 10, x: 10},
    {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
    {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
    {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
    {_id: 20, x: [[{y: {z: 1}}, {y: 2}], {y: 3}, {y: {z: 2}}, [[[{y: 5}, {y: {z: 3}}]]], {y: 6}]},
];
assert.commandWorked(coll.insert(documents));

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({z: 1}));

function checkQuery(
    {expected = [], query = {}, proj = {}, sort = null, limit = null, skip = null, desc = null},
    hint) {
    let findCommand = coll.find(query, proj);
    if (sort) {
        findCommand = findCommand.sort(sort);
    }
    if (limit) {
        findCommand = findCommand.limit(limit);
    }
    if (skip) {
        findCommand = findCommand.skip(skip);
    }
    if (hint) {
        findCommand = findCommand.hint(hint);
    }

    let results = findCommand.toArray();

    function operationDescription() {
        const result = {query: query};
        if (sort) {
            result.sort = sort;
        }
        if (proj) {
            result.proj = proj;
        }
        if (limit) {
            result.limit = limit;
        }
        if (skip) {
            result.skip = skip;
        }
        if (hint) {
            result.hint = hint;
        }
        if (desc) {
            result.desc = desc;
        }
        return tojson(result);
    }
    if (sort) {
        assert(orderedArrayEq(results, expected),
               `operation=${operationDescription()}, actual=${tojson(results)}, expected=${
                   tojson(expected)}`);
    } else {
        assert(arrayEq(results, expected),
               `operation=${operationDescription()}, actual=${tojson(results)}, expected=${
                   tojson(expected)}`);
    }
}

function documentsWithExcludedField(...fieldNames) {
    return documents.map(doc => {
        const copy = Object.assign({}, doc);
        fieldNames.forEach(name => {
            delete copy[name];
        });
        return copy;
    });
}

// Test the IDHack plan. There's no way to hint IDHack, but we trust that the planner will choose it
// for this query.
function runIDHackTest() {
    checkQuery(
        {desc: "_id point query", expected: [{_id: 1, a: 2, b: "y", c: 11}], query: {_id: 1}});
}

// These tests are intended to validate covered projections, so we prevent covered plans by
// requesting a collection scans.
function runCollScanTests() {
    const testCases = [
        //
        // Simple projections
        //
        {
            desc: "_id-only projection",
            expected: documents.map(doc => ({_id: doc._id})),
            proj: {_id: 1}
        },
        {
            desc: "Single-field projection 1",
            expected: [
                {_id: 0, a: 1},   {_id: 1, a: 2},   {_id: 2, a: 3}, {_id: 3},  {_id: 4},
                {_id: 5},         {_id: 6},         {_id: 7},       {_id: 8},  {_id: 9},
                {_id: 10},        {_id: 11},        {_id: 12},      {_id: 13}, {_id: 14},
                {_id: 15, a: 10}, {_id: 16, a: 10}, {_id: 17},      {_id: 18}, {_id: 19},
                {_id: 20}
            ],
            proj: {a: 1}
        },
        {
            desc: "Single-field projection 2",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3},
                {_id: 4},
                {_id: 5},
                {_id: 6},
                {_id: 7},
                {_id: 8},
                {_id: 9},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15},
                {_id: 16},
                {_id: 17},
                {_id: 18},
                {_id: 19},
                {_id: 20}
            ],
            proj: {z: 1}
        },
        {
            desc: "Two-field projection",
            expected: [
                {_id: 0, a: 1, b: "x"},
                {_id: 1, a: 2, b: "y"},
                {_id: 2, a: 3, b: "z"},
                {_id: 3},
                {_id: 4},
                {_id: 5},
                {_id: 6},
                {_id: 7},
                {_id: 8},
                {_id: 9},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15, a: 10},
                {_id: 16, a: 10},
                {_id: 17},
                {_id: 18},
                {_id: 19},
                {_id: 20}
            ],
            proj: {b: 1, a: 1}
        },
        {
            desc: "Projection excluding _id",
            expected: [
                {a: 1}, {a: 2}, {a: 3}, {}, {},      {},      {}, {}, {}, {}, {},
                {},     {},     {},     {}, {a: 10}, {a: 10}, {}, {}, {}, {}
            ],
            proj: {a: 1, _id: 0}
        },
        {
            desc: "$gt query with single-field projection",
            expected: [{_id: 1, a: 2}, {_id: 2, a: 3}, {_id: 15, a: 10}, {_id: 16, a: 10}],
            query: {a: {$gt: 1}},
            proj: {a: 1}
        },
        {
            desc: "Projection of missing field",
            expected: [
                {_id: 0, a: 1},   {_id: 1, a: 2},   {_id: 2, a: 3}, {_id: 3},  {_id: 4},
                {_id: 5},         {_id: 6},         {_id: 7},       {_id: 8},  {_id: 9},
                {_id: 10},        {_id: 11},        {_id: 12},      {_id: 13}, {_id: 14},
                {_id: 15, a: 10}, {_id: 16, a: 10}, {_id: 17},      {_id: 18}, {_id: 19},
                {_id: 20}
            ],
            proj: {a: 1, nonexistent: 1}
        },
        //
        // Dotted-path projections.
        //
        {
            desc: "Dotted-path projection explicitly including _id",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}},
                {_id: 6, x: {y: 4}},
                {_id: 7, x: [{y: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}]},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15},
                {_id: 16},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            proj: {_id: 1, "x.y": 1}
        },
        {
            desc: "Dotted-path projection implicitly including _id",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}},
                {_id: 6, x: {y: 4}},
                {_id: 7, x: [{y: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}]},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15},
                {_id: 16},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            proj: {"x.y": 1}
        },
        {
            desc: "Dotted-path projection excluding _id",
            expected: [
                {},
                {},
                {},
                {x: {y: 1}},
                {x: {y: 2}},
                {x: {y: [1, 2, 3]}},
                {x: {y: 4}},
                {x: [{y: 1}]},
                {x: [{y: 1}, {y: 2}]},
                {x: [{y: 1}, {y: [1, 2, 3]}]},
                {},
                {},
                {},
                {},
                {},
                {},
                {},
                {x: {y: [{z: 1}, {z: 2}]}},
                {x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {x: [[{y: {z: 1}}, {y: 2}], {y: 3}, {y: {z: 2}}, [[[{y: 5}, {y: {z: 3}}]]], {y: 6}]}

            ],
            proj: {_id: 0, "x.y": 1}
        },
        {
            desc: "Three-level dotted-path projection",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3, x: {}},
                {_id: 4, x: {}},
                {_id: 5, x: {y: []}},
                {_id: 6, x: {}},
                {_id: 7, x: [{}]},
                {_id: 8, x: [{}, {}]},
                {_id: 9, x: [{}, {y: []}]},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15},
                {_id: 16},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 19, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {
                    _id: 20,
                    x: [
                        [
                            {y: {z: 1}},
                            {

                            }
                        ],
                        {

                        },
                        {y: {z: 2}},
                        [[[
                            {

                            },
                            {y: {z: 3}}
                        ]]],
                        {

                        }
                    ]
                }
            ],
            proj: {"x.y.z": 1}
        },
        {
            desc: "Two-level dotted-path projection",
            expected: [
                {_id: 0},  {_id: 1},  {_id: 2},  {_id: 3},  {_id: 4},  {_id: 5},         {_id: 6},
                {_id: 7},  {_id: 8},  {_id: 9},  {_id: 10}, {_id: 11}, {_id: 12, z: []}, {_id: 13},
                {_id: 14}, {_id: 15}, {_id: 16}, {_id: 17}, {_id: 18}, {_id: 19},        {_id: 20}
            ],
            proj: {"z.a": 1}
        },
        {
            desc: "Two two-level dotted-path projections",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {y: 4}, v: {w: 4}},
                {_id: 7, x: [{y: 1}], v: [{w: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15},
                {_id: 16},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            proj: {"x.y": 1, "v.w": 1}
        },
        {
            desc: "$gt query with two-level dotted-path projection",
            expected: [
                {_id: 4},
                {_id: 5, v: {w: [4, 5, 6]}},
                {_id: 6, v: {w: 4}},
                {_id: 8, v: [{w: 5}, {w: 6}]},
                {_id: 9, v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 18},
                {_id: 19},
                {_id: 20},
            ],
            query: {'x.y': {$gt: 1}},
            proj: {"v.w": 1}
        },
        {
            desc: "Three-level dotted-path component with missing field",
            expected: [
                {_id: 0},
                {_id: 1},
                {_id: 2},
                {_id: 3, x: {}},
                {_id: 4, x: {}},
                {_id: 5, x: {y: []}},
                {_id: 6, x: {}},
                {_id: 7, x: [{}]},
                {_id: 8, x: [{}, {}]},
                {_id: 9, x: [{}, {y: []}]},
                {_id: 10},
                {_id: 11},
                {_id: 12},
                {_id: 13},
                {_id: 14},
                {_id: 15},
                {_id: 16},
                {_id: 17, x: {y: [{}, {}]}},
                {_id: 18, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 19, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 20, x: [[{y: {}}, {}], {}, {y: {}}, [[[{}, {y: {}}]]], {}]}
            ],
            proj: {"x.y.nonexistent": 1}
        },
        {
            desc: "Dotted-path exclusion projection explicitly including _id",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {}},
                {_id: 4, x: {}},
                {_id: 5, x: {}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {}, v: {w: 4}},
                {_id: 7, x: [{}], v: [{w: 1}]},
                {_id: 8, x: [{}, {}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{}, {}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {}},
                {_id: 18, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 19, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 20, x: [[{}, {}], {}, {}, [[[{}, {}]]], {}]}
            ],
            proj: {_id: 1, "x.y": 0}
        },
        {
            desc: "Dotted-path exclusion projection implicitly including _id",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {}},
                {_id: 4, x: {}},
                {_id: 5, x: {}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {}, v: {w: 4}},
                {_id: 7, x: [{}], v: [{w: 1}]},
                {_id: 8, x: [{}, {}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{}, {}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {}},
                {_id: 18, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 19, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 20, x: [[{}, {}], {}, {}, [[[{}, {}]]], {}]}
            ],
            proj: {"x.y": 0}
        },
        {
            desc: "Dotted-path exclusion projection excluding _id",
            expected: [
                {a: 1, b: "x", c: 10},
                {a: 2, b: "y", c: 11},
                {a: 3, b: "z", c: 12},
                {x: {}},
                {x: {}},
                {x: {}, v: {w: [4, 5, 6]}},
                {x: {}, v: {w: 4}},
                {x: [{}], v: [{w: 1}]},
                {x: [{}, {}], v: [{w: 5}, {w: 6}]},
                {x: [{}, {}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {z: 1},
                {z: 2},
                {z: [1, 2, 3]},
                {z: 3},
                {z: 4},
                {a: 10, x: 1},
                {a: 10, x: 10},
                {x: {}},
                {x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {x: [[{}, {}], {}, {}, [[[{}, {}]]], {}]}
            ],
            proj: {_id: 0, "x.y": 0}
        },
        {
            desc: "Three-level dotted-path exclusion projection",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {y: 4}, v: {w: 4}},
                {_id: 7, x: [{y: 1}], v: [{w: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {y: [{}, {}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 20, x: [[{y: {}}, {y: 2}], {y: 3}, {y: {}}, [[[{y: 5}, {y: {}}]]], {y: 6}]}
            ],
            proj: {"x.y.z": 0}
        },
        {
            desc: "Two-level dotted-path exclusion projection",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {y: 4}, v: {w: 4}},
                {_id: 7, x: [{y: 1}], v: [{w: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            proj: {"z.a": 0}
        },
        {
            desc: "Exclusion projection with two dotted paths",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {}},
                {_id: 4, x: {}},
                {_id: 5, x: {}, v: {}},
                {_id: 6, x: {}, v: {}},
                {_id: 7, x: [{}], v: [{}]},
                {_id: 8, x: [{}, {}], v: [{}, {}]},
                {_id: 9, x: [{}, {}], v: [{}, {}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {}},
                {_id: 18, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 19, x: [[{}, {}], {}, {}, [[[{}]]], {}]},
                {_id: 20, x: [[{}, {}], {}, {}, [[[{}, {}]]], {}]}
            ],
            proj: {"x.y": 0, "v.w": 0}
        },
        {
            desc: "$gt query with two-level dotted-path exclusion projection",
            expected: [
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}, v: {}},
                {_id: 6, x: {y: 4}, v: {}},
                {_id: 8, x: [{y: 1}, {y: 2}], v: [{}, {}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{}, {}]},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            query: {"x.y": {$gt: 1}},
            proj: {"v.w": 0}
        },
        {
            desc: "Three-level dotted-path exclusion projection with missing field",
            expected: [
                {_id: 0, a: 1, b: "x", c: 10},
                {_id: 1, a: 2, b: "y", c: 11},
                {_id: 2, a: 3, b: "z", c: 12},
                {_id: 3, x: {y: 1}},
                {_id: 4, x: {y: 2}},
                {_id: 5, x: {y: [1, 2, 3]}, v: {w: [4, 5, 6]}},
                {_id: 6, x: {y: 4}, v: {w: 4}},
                {_id: 7, x: [{y: 1}], v: [{w: 1}]},
                {_id: 8, x: [{y: 1}, {y: 2}], v: [{w: 5}, {w: 6}]},
                {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], v: [{w: 4}, {w: [4, 5, 6]}]},
                {_id: 10, z: 1},
                {_id: 11, z: 2},
                {_id: 12, z: [1, 2, 3]},
                {_id: 13, z: 3},
                {_id: 14, z: 4},
                {_id: 15, a: 10, x: 1},
                {_id: 16, a: 10, x: 10},
                {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
                {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {_id: 19, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
                {
                    _id: 20,
                    x: [
                        [{y: {z: 1}}, {y: 2}],
                        {y: 3},
                        {y: {z: 2}},
                        [[[{y: 5}, {y: {z: 3}}]]],
                        {y: 6}
                    ]
                }
            ],
            proj: {"x.y.nonexistent": 0}
        },
        //
        // Simple exclusion projections.
        //
        {
            desc: "_id-exclusion projection",
            expected: documentsWithExcludedField("_id"),
            proj: {_id: 0}
        },
        {
            desc: "Single-field exclusion projection 1",
            expected: documentsWithExcludedField("a"),
            proj: {a: 0}
        },
        {
            desc: "Single-field exclusion projection 2",
            expected: documentsWithExcludedField("z"),
            proj: {z: 0}
        },
        {
            desc: "Exclusion projection with two fields",
            expected: documentsWithExcludedField("b", "a"),
            proj: {b: 0, a: 0}
        },
        {
            desc: "Exclusion projection explicitly including _id",
            expected: documentsWithExcludedField("a"),
            proj: {a: 0, _id: 1}
        },
        {
            desc: "Exclusion projection with missing field",
            expected: documentsWithExcludedField("a", "nonexistent"),
            proj: {a: 0, nonexistent: 0}
        }
    ];

    testCases.forEach(test => checkQuery(test, {$natural: 1}));
}

function runFindTestsWithHint(hint) {
    // Note that sorts are chosen so that results are deterministic. Either there are no ties, or
    // any tied documents are filted out by a "limit" parameter.
    const testCases = [
        {desc: "All-document query", expected: documents, query: {}},
        {desc: "Point query 1", expected: [documents[1]], query: {a: 2}},
        {
            desc: "$gt query",
            expected: [documents[1], documents[2], documents[15], documents[16]],
            query: {a: {$gt: 1}}
        },
        {
            desc: "$gte query",
            expected: [documents[0], documents[1], documents[2], documents[15], documents[16]],
            query: {a: {$gte: 1}}
        },
        {desc: "$lt query", expected: [], query: {a: {$lt: 1}}},
        {desc: "$lte query", expected: [documents[0]], query: {a: {$lte: 1}}},
        {desc: "(Range) query", expected: [documents[1]], query: {a: {$gt: 1, $lt: 3}}},
        {
            desc: "(Range] query",
            expected: [documents[1], documents[2]],
            query: {a: {$gt: 1, $lte: 3}}
        },
        {
            desc: "[Range) query",
            expected: [documents[0], documents[1]],
            query: {a: {$gte: 1, $lt: 3}}
        },
        {
            desc: "[Range] query",
            expected: [documents[0], documents[1], documents[2]],
            query: {a: {$gte: 1, $lte: 3}}
        },
        {desc: "Query with implicit conjunction", expected: [documents[15]], query: {a: 10, x: 1}},
        {
            desc: "$lt query with sort",
            expected: [documents[0], documents[1]],
            query: {a: {$lt: 3}},
            sort: {a: 1}
        },
        {
            desc: "$lte query with compound sort 1",
            expected: [documents[0], documents[1], documents[2]],
            query: {a: {$lte: 3}},
            sort: {a: 1, b: 1}
        },
        {
            desc: "$lte query with compound sort 2",
            expected: [documents[0], documents[1], documents[2]],
            query: {a: {$lte: 3}},
            sort: {b: 1, a: 1}
        },
        {expected: [documents[0]], query: {a: {$lt: 3}}, sort: {a: 1}, limit: 1},
        {
            desc: "$lte query with compound sort and limit",
            expected: [documents[0]],
            query: {a: {$lte: 3}},
            sort: {a: 1, b: 1},
            limit: 1,
        },
        {
            desc: "Point query with limit",
            expected: [documents[0]],
            query: {a: 1},
            limit: 2,
            hint: hint
        },
        {desc: "Point query siwth skip", expected: [], query: {a: 1}, skip: 2},
        {desc: "Point query siwth skip and limit", expected: [], query: {a: 1}, skip: 1, limit: 1},
        {desc: "Point query 2", expected: [documents[11], documents[12]], query: {z: 2}},
        {
            desc: "Query on dotted path",
            expected: [documents[4], documents[5], documents[8], documents[9]],
            query: {"x.y": 2}
        },
        {desc: "Query on dotted path returning no documents", expected: [], query: {"x.y": 5}}
    ];

    testCases.forEach(test => checkQuery(test, hint));
}

runIDHackTest();

runCollScanTests();

runFindTestsWithHint({$natural: 1});
runFindTestsWithHint({a: 1});
runFindTestsWithHint({z: 1});  // Multi-key
}());
