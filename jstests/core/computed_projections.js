// @tags: [
//   # Can't set the 'failOnPoisonedFieldLookup' failpoint on mongos.
//   assumes_against_mongod_not_mongos,
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq and orderedArrayEq.

if (assert
        .commandWorked(
            db.adminCommand({getParameter: 1, internalQueryEnableSlotBasedExecutionEngine: 1}))
        .internalQueryEnableSlotBasedExecutionEngine) {
    // Override error-code-checking APIs. We only load this when SBE is explicitly enabled, because
    // it causes failures in the parallel suites.
    load("jstests/libs/sbe_assert_error_override.js");
}

// It is safe for other tests to run while this failpoint is active, so long as those tests do not
// use documents containing a field with "POISON" as their name.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "failOnPoisonedFieldLookup", mode: "alwaysOn"}));

const coll = db.computed_projection;
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
    {_id: 19, i: {j: 5}, k: {l: 10}},
    {_id: 20, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
    {_id: 21, x: [[{y: {z: 1}}, {y: 2}], {y: 3}, {y: {z: 2}}, [[[{y: 5}, {y: {z: 3}}]]], {y: 6}]},
    {_id: 22, tf: [true, false], ff: [false, false], t: true, f: false, n: null, a: 1, b: 0},
    {_id: 23, i1: NumberInt(1), i2: NumberInt(-1), i3: NumberInt(-2147483648)},
    {_id: 24, l1: NumberLong("12345678900"), l2: NumberLong("-12345678900")},
    {_id: 25, s: "string", l: NumberLong("-9223372036854775808"), n: null},
    {_id: 26, d1: 4.6, d2: -4.6, dec1: NumberDecimal("4.6"), dec2: NumberDecimal("-4.6")}
];
assert.commandWorked(coll.insert(documents));

// A concise way to express an "expected" result that takes the form
// [{_id: 0, foo: <BOOL>}, {_id, 1, foo: <BOOL>}, ..., {_id: 26, foo: <BOOL>}] by passing an object
// of the form {foo: [INTEGER_LIST]}, where INTEGER_LIST is the list of '_id' values for documents
// where "foo" should be true.
function computedProjectionWithBoolValues(boolProj) {
    return documents.map(doc => {
        const projectedDoc = {_id: doc._id};
        Object.keys(boolProj).forEach(key => {
            projectedDoc[key] = boolProj[key].includes(doc._id);
        });
        return projectedDoc;
    });
}

const testCases = [
    {
        desc: "Single-level path 1",
        expected: [
            {_id: 0, foo: 1},   {_id: 1, foo: 2},   {_id: 2, foo: 3},  {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},           {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},          {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15, foo: 10}, {_id: 16, foo: 10}, {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},          {_id: 22, foo: 1}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {_id: 1, foo: "$a"}
    },
    {
        desc: "Single-level path 2",
        expected: [
            {_id: 0, foo: 1},   {_id: 1, foo: 2},   {_id: 2, foo: 3},  {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},           {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},          {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15, foo: 10}, {_id: 16, foo: 10}, {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},          {_id: 22, foo: 1}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {foo: "$a"}
    },
    {
        desc: "Single-level path 3",
        expected: [
            {foo: 1}, {foo: 2},  {foo: 3},  {}, {}, {}, {}, {}, {},       {}, {}, {}, {}, {},
            {},       {foo: 10}, {foo: 10}, {}, {}, {}, {}, {}, {foo: 1}, {}, {}, {}, {}
        ],
        query: {},
        proj: {_id: 0, foo: "$a"}
    },
    {
        desc: "Two single-level paths",
        expected: [
            {_id: 0, foo: 1, bar: "x"},
            {_id: 1, foo: 2, bar: "y"},
            {_id: 2, foo: 3, bar: "z"},
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
            {_id: 15, foo: 10},
            {_id: 16, foo: 10},
            {_id: 17},
            {_id: 18},
            {_id: 19},
            {_id: 20},
            {_id: 21},
            {_id: 22, foo: 1, bar: 0},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {_id: 1, foo: "$a", bar: "$b"}
    },
    {
        desc: "Simple addition",
        expected: [{_id: 0, foo: 11}, {_id: 1, foo: 13}, {_id: 2, foo: 15}],
        query: {c: {$gt: 0}},
        proj: {foo: {$add: ["$a", "$c"]}}
    },
    {
        desc: "Single-level path 4",
        expected: [
            {_id: 0, a: 1, foo: "x"},
            {_id: 1, a: 2, foo: "y"},
            {_id: 2, a: 3, foo: "z"},
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
            {_id: 20},
            {_id: 21},
            {_id: 22, a: 1, foo: 0},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {a: 1, foo: "$b"}
    },
    {
        desc: "Two-level path 1",
        expected: [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3, foo: 1},
            {_id: 4, foo: 2},
            {_id: 5, foo: [1, 2, 3]},
            {_id: 6, foo: 4},
            {_id: 7, foo: [1]},
            {_id: 8, foo: [1, 2]},
            {_id: 9, foo: [1, [1, 2, 3]]},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
            {_id: 17, foo: [{z: 1}, {z: 2}]},
            {_id: 18, foo: [3, 4, 6]},
            {_id: 19},
            {_id: 20, foo: [3, 4, 6]},
            {_id: 21, foo: [3, {z: 2}, 6]},
            {_id: 22},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {_id: 1, foo: "$x.y"}
    },
    {
        desc: "Two-level path 2",
        expected: [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3, foo: 1},
            {_id: 4, foo: 2},
            {_id: 5, foo: [1, 2, 3]},
            {_id: 6, foo: 4},
            {_id: 7, foo: [1]},
            {_id: 8, foo: [1, 2]},
            {_id: 9, foo: [1, [1, 2, 3]]},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
            {_id: 17, foo: [{z: 1}, {z: 2}]},
            {_id: 18, foo: [3, 4, 6]},
            {_id: 19},
            {_id: 20, foo: [3, 4, 6]},
            {_id: 21, foo: [3, {z: 2}, 6]},
            {_id: 22},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {foo: "$x.y"}
    },
    {
        desc: "Two-level path 3",
        expected: [
            {},
            {},
            {},
            {foo: 1},
            {foo: 2},
            {foo: [1, 2, 3]},
            {foo: 4},
            {foo: [1]},
            {foo: [1, 2]},
            {foo: [1, [1, 2, 3]]},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {foo: [{z: 1}, {z: 2}]},
            {foo: [3, 4, 6]},
            {},
            {foo: [3, 4, 6]},
            {foo: [3, {z: 2}, 6]},
            {},
            {},
            {},
            {},
            {}
        ],
        query: {},
        proj: {_id: 0, foo: "$x.y"}
    },
    {
        desc: "Two two-level paths",
        expected: [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3, foo: 1},
            {_id: 4, foo: 2},
            {_id: 5, foo: [1, 2, 3], bar: [4, 5, 6]},
            {_id: 6, foo: 4, bar: 4},
            {_id: 7, foo: [1], bar: [1]},
            {_id: 8, foo: [1, 2], bar: [5, 6]},
            {_id: 9, foo: [1, [1, 2, 3]], bar: [4, [4, 5, 6]]},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
            {_id: 17, foo: [{z: 1}, {z: 2}]},
            {_id: 18, foo: [3, 4, 6]},
            {_id: 19},
            {_id: 20, foo: [3, 4, 6]},
            {_id: 21, foo: [3, {z: 2}, 6]},
            {_id: 22},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {foo: "$x.y", bar: "$v.w"}
    },
    {
        desc: "Addition of two-level paths",
        expected: [{_id: 19, foo: 15}],
        query: {"i.j": {$gt: 0}},
        proj: {foo: {$add: ["$i.j", "$k.l"]}}
    },
    {
        desc: "Dotted-path projection and two-level path",
        expected: [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3, x: {y: 1}},
            {_id: 4, x: {y: 2}},
            {_id: 5, x: {y: [1, 2, 3]}, foo: [4, 5, 6]},
            {_id: 6, x: {y: 4}, foo: 4},
            {_id: 7, x: [{y: 1}], foo: [1]},
            {_id: 8, x: [{y: 1}, {y: 2}], foo: [5, 6]},
            {_id: 9, x: [{y: 1}, {y: [1, 2, 3]}], foo: [4, [4, 5, 6]]},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
            {_id: 17, x: {y: [{z: 1}, {z: 2}]}},
            {_id: 18, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
            {_id: 19},
            {_id: 20, x: [[{y: 1}, {y: 2}], {y: 3}, {y: 4}, [[[{y: 5}]]], {y: 6}]},
            {
                _id: 21,
                x: [[{y: {z: 1}}, {y: 2}], {y: 3}, {y: {z: 2}}, [[[{y: 5}, {y: {z: 3}}]]], {y: 6}]
            },
            {_id: 22},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {"x.y": 1, foo: "$v.w"}
    },
    //
    // Test simple expressions with the $abs operator.
    //
    {
        desc: "$abs operator",
        expected: [{_id: 23, abs_i1: 1, abs_i2: 1, abs_i3: NumberLong("2147483648")}],
        query: {i1: 1},
        proj: {abs_i1: {$abs: "$i1"}, abs_i2: {$abs: "$i2"}, abs_i3: {$abs: "$i3"}}
    },
    {
        desc: "$abs with NumberLong input",
        expected: [{_id: 24, abs_l1: NumberLong("12345678900"), abs_l2: NumberLong("12345678900")}],
        query: {l1: NumberLong("12345678900")},
        proj: {abs_l1: {$abs: "$l1"}, abs_l2: {$abs: "$l2"}}
    },
    {
        desc: "$abs with NumberDecimal input",
        expected: [{
            _id: 26,
            abs_d1: 4.6,
            abs_d2: 4.6,
            abs_dec1: NumberDecimal("4.6"),
            abs_dec2: NumberDecimal("4.6")
        }],
        query: {d1: 4.6},
        proj: {
            abs_d1: {$abs: "$d1"},
            abs_d2: {$abs: "$d2"},
            abs_dec1: {$abs: "$dec1"},
            abs_dec2: {$abs: "$dec2"}
        }
    },
    {
        desc: "$abs with string input",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {abs_s: {$abs: "$s"}}
    },
    {
        desc: "$abs with MIN_LONG_LONG input",
        expectedErrorCode: 28680,
        query: {s: "string"},
        proj: {abs_l: {$abs: "$l"}}
    },
    {
        desc: "$abs with missing input",
        expected: [{_id: 25, abs_n: null, abs_ne: null}],
        query: {s: "string"},
        proj: {abs_n: {$abs: "$n"}, abs_ne: {$abs: "$non_existent"}}
    },
    //
    // Test $and/$or.
    //
    {
        desc: "Single-branch $and",
        expected: [
            {_id: 0, foo: true},   {_id: 1, foo: true},   {_id: 2, foo: true},
            {_id: 3, foo: false},  {_id: 4, foo: false},  {_id: 5, foo: false},
            {_id: 6, foo: false},  {_id: 7, foo: false},  {_id: 8, foo: false},
            {_id: 9, foo: false},  {_id: 10, foo: false}, {_id: 11, foo: false},
            {_id: 12, foo: false}, {_id: 13, foo: false}, {_id: 14, foo: false},
            {_id: 15, foo: true},  {_id: 16, foo: true},  {_id: 17, foo: false},
            {_id: 18, foo: false}, {_id: 19, foo: false}, {_id: 20, foo: false},
            {_id: 21, foo: false}, {_id: 22, foo: true},  {_id: 23, foo: false},
            {_id: 24, foo: false}, {_id: 25, foo: false}, {_id: 26, foo: false}
        ],
        query: {},
        proj: {foo: {$and: ["$a"]}}
    },
    {
        desc: "Single-branch $or",
        expected: [
            {_id: 0, foo: true},   {_id: 1, foo: true},   {_id: 2, foo: true},
            {_id: 3, foo: false},  {_id: 4, foo: false},  {_id: 5, foo: false},
            {_id: 6, foo: false},  {_id: 7, foo: false},  {_id: 8, foo: false},
            {_id: 9, foo: false},  {_id: 10, foo: false}, {_id: 11, foo: false},
            {_id: 12, foo: false}, {_id: 13, foo: false}, {_id: 14, foo: false},
            {_id: 15, foo: true},  {_id: 16, foo: true},  {_id: 17, foo: false},
            {_id: 18, foo: false}, {_id: 19, foo: false}, {_id: 20, foo: false},
            {_id: 21, foo: false}, {_id: 22, foo: true},  {_id: 23, foo: false},
            {_id: 24, foo: false}, {_id: 25, foo: false}, {_id: 26, foo: false}
        ],
        query: {},
        proj: {foo: {$or: ["$a"]}}
    },
    {
        desc: "$and with BSONNull branch",
        expected: [{_id: 22, foo: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: ["$n"]}}
    },
    {
        desc: "$or with BSONNull branch",
        expected: [{_id: 22, foo: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$or: ["$n"]}}
    },
    {
        desc: "$and with missing path in branch",
        expected: [{_id: 22, foo: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: ["$nonexistent"]}}
    },
    {
        desc: "$or with missing path in branch",
        expected: [{_id: 22, foo: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$or: ["$nonexistent"]}}
    },
    {
        desc: "$and with array branch",
        expected: [{_id: 22, foo: true, bar: true}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: []}, bar: {$and: ["$tf", "$t", "$a"]}}
    },
    {
        desc: "Three-branch $or",
        expected: [{_id: 22, foo: false, bar: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$or: []}, bar: {$or: ["$f", "$n", "$nonexistent"]}}
    },
    {
        desc: "Multiple computed $and projections 1",
        expected: [{_id: 22, foo: false, bar: false, baz: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: ["$a", "$b"]}, bar: {$and: ["$a", "$f"]}, baz: {$and: ["$a", "$n"]}}
    },
    {
        desc: "Multiple computed $or projections",
        expected: [{_id: 22, foo: true, bar: true, baz: true}],
        query: {_id: 22, a: 1},
        proj: {foo: {$or: ["$a", "$b"]}, bar: {$or: ["$a", "$f"]}, baz: {$or: ["$a", "$n"]}}
    },
    {
        desc: "Multiple computed $and projections 2",
        expected: [{_id: 22, foo: true, bar: false}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: ["$ff", "$t"]}, bar: {$and: ["$nonexistent", "$t"]}}
    },
    //
    // $switch and $cond tests.
    //
    {
        desc: "Single-case $switch with default",
        expected: [
            {_id: 0, foo: "x"}, {_id: 1, foo: 11}, {_id: 2, foo: 12}, {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},          {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},         {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15},          {_id: 16},         {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},         {_id: 22, foo: 0}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {foo: {$switch: {branches: [{case: {$eq: ["$a", 1]}, then: "$b"}], default: "$c"}}}
    },
    {
        desc: "$cond",
        expected: [
            {_id: 0, foo: "x"}, {_id: 1, foo: 11}, {_id: 2, foo: 12}, {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},          {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},         {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15},          {_id: 16},         {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},         {_id: 22, foo: 0}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {foo: {$cond: {if: {$eq: ["$a", 1]}, then: "$b", else: "$c"}}}
    },
    {
        desc: "Two-case $switch with default",
        expected: [
            {_id: 0, foo: "x"}, {_id: 1, foo: 2}, {_id: 2, foo: 12}, {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},         {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},        {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15},          {_id: 16},        {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},        {_id: 22, foo: 0}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {
            foo: {
                $switch: {
                    branches: [
                        {case: {$eq: ["$a", 1]}, then: "$b"},
                        {case: {$eq: ["$b", "y"]}, then: "$a"}
                    ],
                    default: "$c"
                }
            }
        }
    },
    //
    // Failing expressions
    //
    {
        desc: "$abs with string input as $and branch",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$and: [{$abs: ["$s"]}, "$n"]}}
    },
    {
        desc: "$abs with string input as $or branch",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$or: [{$abs: ["$s"]}, "$s"]}}
    },
    {
        desc: "Switch fall-through with no default",
        expectedErrorCode: 40066,
        query: {},
        proj: {
            foo: {
                $switch: {
                    branches: [
                        {case: {$eq: ["$a", 1]}, then: "$b"},
                        {case: {$eq: ["$b", "y"]}, then: "$a"}
                    ]
                    // No default case.
                }
            }
        }
    },
    {
        desc: "$abs with string input as case condition",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$switch: {branches: [{case: {$gt: [{$abs: ["$s"]}, 0]}, then: "$n"}]}}}
    },
    {
        desc: "$abs with string input as case result",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$switch: {branches: [{case: {$eq: ["$s", "string"]}, then: {$abs: ["$s"]}}]}}}
    },
    {
        desc: "$abs with string input as $switch default",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {
            foo:
                {$switch: {branches: [{case: {$eq: ["$s", 0]}, then: "$n"}], default: {$abs: "$s"}}}
        }
    },
    {
        desc: "$abs with string input as $cond condition",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$cond: {if: {$eq: [{$abs: "$s"}, "$n"]}, then: "$b", else: "$c"}}}
    },
    {
        desc: "$abs with string input as $cond result (then)",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$cond: {if: {$eq: ["$s", "string"]}, then: {$abs: ["$s"]}, else: "$c"}}}
    },
    {
        desc: "$abs with string input as $cond result (else)",
        expectedErrorCode: 28765,
        query: {s: "string"},
        proj: {foo: {$cond: {if: {$eq: ["$s", "gnirts"]}, then: "$b", else: {$abs: ["$s"]}}}}
    },
    //
    // Test short circuiting: these queries have expressions that would fail (because |$x| is
    // invalid) but won't because they do not execute.
    //
    {
        desc: "$abs with string input as short-circuited $and branch",
        expected: [{_id: 25, foo: false}],
        query: {s: "string"},
        proj: {foo: {$and: ["$n", {$abs: ["$s"]}]}}
    },
    {
        desc: "$abs with string input as short-circuited $or branch",
        expected: [{_id: 25, foo: true}],
        query: {s: "string"},
        proj: {foo: {$or: ["$s", {$abs: ["$s"]}]}}
    },
    //
    // Test that short-circuited branches do not do any work, such as traversing an array. The
    // 'failOnPoisonedFieldLookup' failpoint ensures that none of the "$POISON" lookups, which are
    // in short-circuited branches, ever execute. If they were to execute, it would result in an
    // error from the query that would cause the test to fail.
    //
    {
        desc: "$POISON in short-circuited $and/$or branches",
        expected: [{_id: 22, foo: false, bar: true}],
        query: {_id: 22, a: 1},
        proj: {foo: {$and: ["$f", "$POISON"]}, bar: {$or: ["$t", "$POISON"]}}
    },
    {
        desc: "$POISON in nested short-circuited $or branches",
        expected: [{_id: 22, foo: false, bar: true}],
        query: {_id: 22, a: 1},
        proj: {
            foo: {$and: ["$f", {$or: ["$f", "$POISON"]}, {$eq: ["$a", 1]}]},
            bar: {$and: ["$t", {$or: ["$t", "$POISON"]}, {$eq: ["$a", 1]}]}
        }
    },
    {
        desc: "$POISON in untaken $switch cases",
        expected: [{_id: 0, foo: "x"}, {_id: 22, foo: 0}],
        query: {a: 1},
        proj: {
            foo: {
                $switch: {
                    branches: [
                        {case: {$eq: ["$a", 2]}, then: "$POISON"},
                        {case: {$eq: ["$a", 3]}, then: "$POISON"}
                    ],
                    default: "$b"
                }
            }
        }
    },
    {
        desc: "$POISON in unevaluated case condition and untaken $switch default branch",
        expected: [{_id: 0, foo: "x"}, {_id: 22, foo: 0}],
        query: {a: 1},
        proj: {
            foo: {
                $switch: {
                    branches:
                        [{case: {$eq: ["$a", 1]}, then: "$b"}, {case: "$POISON", then: "$POISON"}],
                    default: "$POISON"
                }
            }
        }
    },
    {
        desc: "$POISON in untaken $cond branch (else)",
        expected: [{_id: 0, foo: "x"}, {_id: 22, foo: 0}],
        query: {a: 1},
        proj: {foo: {$cond: {if: {$eq: ["$a", 1]}, then: "$b", else: "$POISON"}}}
    },
    {
        desc: "$POISON in untaken $cond branch (then)",
        expected: [{_id: 0, foo: "x"}, {_id: 22, foo: 0}],
        query: {a: 1},
        proj: {foo: {$cond: {if: {$eq: ["$a", 2]}, then: "$POISON", else: "$b"}}}
    },
    //
    // $let tests.
    //
    {
        desc: "$let with single-path variable definitions 1",
        expected: computedProjectionWithBoolValues({foo: [0, 1, 2, 15, 16, 22]}),
        query: {},
        proj: {foo: {$let: {vars: {va: "$a", vb: "$b"}, "in": {$and: "$$va"}}}}
    },
    {
        desc: "$let with single-path variable definitions 2",
        expected: computedProjectionWithBoolValues({foo: [0, 1, 2]}),
        query: {},
        proj: {foo: {$let: {vars: {va: "$a", vb: "$b"}, "in": {$and: "$$vb"}}}}
    },
    {
        desc: "$let with single-path variable definitions 3",
        expected: computedProjectionWithBoolValues({foo: [0, 1, 2]}),
        query: {},
        proj: {foo: {$let: {vars: {va: "$a", vb: "$b"}, "in": {$and: ["$$va", "$$vb"]}}}}
    },
    {
        desc: "$let with $and variable definition",
        expected: computedProjectionWithBoolValues({foo: [0, 1, 2]}),
        query: {},
        proj: {foo: {$let: {vars: {va: {$and: ["$a", "$b"]}}, "in": "$$va"}}}
    },
    {
        desc: "Two-level path including $let variable",
        expected: [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3, foo: 1},
            {_id: 4, foo: 2},
            {_id: 5, foo: [1, 2, 3]},
            {_id: 6, foo: 4},
            {_id: 7, foo: [1]},
            {_id: 8, foo: [1, 2]},
            {_id: 9, foo: [1, [1, 2, 3]]},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
            {_id: 17, foo: [{z: 1}, {z: 2}]},
            {_id: 18, foo: [3, 4, 6]},
            {_id: 19},
            {_id: 20, foo: [3, 4, 6]},
            {_id: 21, foo: [3, {z: 2}, 6]},
            {_id: 22},
            {_id: 23},
            {_id: 24},
            {_id: 25},
            {_id: 26}
        ],
        query: {},
        proj: {foo: {$let: {vars: {va: "$x"}, "in": "$$va.y"}}}
    },
    {
        desc: "Nested $let expressions",
        expected: computedProjectionWithBoolValues({foo: [22]}),
        query: {},
        proj: {
            foo: {
                $let: {
                    vars: {vt: "$t", vf: "$f"},
                    "in": {
                        $let: {vars: {vf: "$$vt", va: "$a"},
                               "in": {$and: ["$$vt", "$$vf", "$$va"]}}
                    }
                }
            }
        }
    },
    {
        desc: "$let with variable definition including nested $let",
        expected: [{_id: 22, foo: false}],
        query: {_id: 22, a: 1},
        proj: {
            foo: {
                $let: {
                    vars:
                        {va: {$let: {vars: {vt: "$t", va: "$va"}, "in": {$and: ["$$vt", "$$va"]}}}},
                    "in": "$$va"
                }
            }
        }
    },
    {
        desc: "$let renaming $$CURRENT",
        expected: [
            {_id: 0, foo: 1},   {_id: 1, foo: 2},   {_id: 2, foo: 3},  {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},           {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},          {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15, foo: 10}, {_id: 16, foo: 10}, {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},          {_id: 22, foo: 1}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {foo: {$let: {vars: {doc: "$$CURRENT"}, "in": "$$doc.a"}}}
    },
    {
        desc: "$let shadowing $$CURRENT",
        expected: [
            {_id: 0, foo: 1},   {_id: 1, foo: 2},   {_id: 2, foo: 3},  {_id: 3},  {_id: 4},
            {_id: 5},           {_id: 6},           {_id: 7},          {_id: 8},  {_id: 9},
            {_id: 10},          {_id: 11},          {_id: 12},         {_id: 13}, {_id: 14},
            {_id: 15, foo: 10}, {_id: 16, foo: 10}, {_id: 17},         {_id: 18}, {_id: 19},
            {_id: 20},          {_id: 21},          {_id: 22, foo: 1}, {_id: 23}, {_id: 24},
            {_id: 25},          {_id: 26}
        ],
        query: {},
        proj: {foo: {$let: {vars: {CURRENT: "$$CURRENT.a"}, "in": "$$CURRENT"}}}
    },
    {
        desc: "$$REMOVE",
        expected: documents.map(doc => ({_id: doc._id})),
        query: {},
        proj: {a: "$$REMOVE"}
    },
    {
        desc: "$$REMOVE with additional path components",
        expected: documents.map(doc => ({_id: doc._id})),
        query: {},
        proj: {a: "$$REMOVE.x.y"}
    },
    {
        desc: "$lt",
        expected: computedProjectionWithBoolValues(
            {foo: [3, 4, 5, 6, 7, 8, 9, 17, 18, 20, 21], bar: [0, 1, 2], baz: [5, 8, 9], qux: []}),
        query: {},
        proj: {
            foo: {$lt: ["$a", "$x"]},
            bar: {$lt: ["$a", "$b"]},
            baz: {$lt: ["$x.y", "$v.w"]},
            qux: {$lt: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$lte",
        expected: computedProjectionWithBoolValues({
            foo: [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26],
            bar: [
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                12, 13, 14, 17, 18, 19, 20, 21, 23, 24, 25, 26
            ],
            baz: [0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 22, 23, 24, 25, 26],
            qux: [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18, 19, 20, 21, 23, 24, 25, 26]
        }),
        query: {},
        proj: {
            foo: {$lte: ["$a", "$x"]},
            bar: {$lte: ["$a", "$b"]},
            baz: {$lte: ["$x.y", "$v.w"]},
            qux: {$lte: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$gt",
        expected: computedProjectionWithBoolValues({
            foo: [0, 1, 2, 15, 22],
            bar: [15, 16, 22],
            baz: [3, 4, 17, 18, 20, 21],
            qux: [0, 1, 2, 15, 16, 22]
        }),
        query: {},
        proj: {
            foo: {$gt: ["$a", "$x"]},
            bar: {$gt: ["$a", "$b"]},
            baz: {$gt: ["$x.y", "$v.w"]},
            qux: {$gt: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$gte",
        expected: computedProjectionWithBoolValues({
            foo: [0, 1, 2, 10, 11, 12, 13, 14, 15, 16, 19, 22, 23, 24, 25, 26],
            bar: [
                3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26
            ],
            baz: [
                0,  1,  2,  3,  4,  6,  7,  10, 11, 12, 13, 14,
                15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26
            ],
            qux: [
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26
            ]
        }),
        query: {},
        proj: {
            foo: {$gte: ["$a", "$x"]},
            bar: {$gte: ["$a", "$b"]},
            baz: {$gte: ["$x.y", "$v.w"]},
            qux: {$gte: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$eq",
        expected: computedProjectionWithBoolValues({
            foo: [10, 11, 12, 13, 14, 16, 19, 23, 24, 25, 26],
            bar: [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18, 19, 20, 21, 23, 24, 25, 26],
            baz: [0, 1, 2, 6, 7, 10, 11, 12, 13, 14, 15, 16, 19, 22, 23, 24, 25, 26],
            qux: [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18, 19, 20, 21, 23, 24, 25, 26]
        }),
        query: {},
        proj: {
            foo: {$eq: ["$a", "$x"]},
            bar: {$eq: ["$a", "$b"]},
            baz: {$eq: ["$x.y", "$v.w"]},
            qux: {$eq: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$ne",
        expected: computedProjectionWithBoolValues({
            foo: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 17, 18, 20, 21, 22],
            bar: [0, 1, 2, 15, 16, 22],
            baz: [3, 4, 5, 8, 9, 17, 18, 20, 21],
            qux: [0, 1, 2, 15, 16, 22]
        }),
        query: {},
        proj: {
            foo: {$ne: ["$a", "$x"]},
            bar: {$ne: ["$a", "$b"]},
            baz: {$ne: ["$x.y", "$v.w"]},
            qux: {$ne: ["$a", "$nonexistent"]}
        }
    },
    {
        desc: "$cmp",
        expected: [
            {_id: 0, foo: 1, bar: -1, baz: 0, qux: 1},  {_id: 1, foo: 1, bar: -1, baz: 0, qux: 1},
            {_id: 2, foo: 1, bar: -1, baz: 0, qux: 1},  {_id: 3, foo: -1, bar: 0, baz: 1, qux: 0},
            {_id: 4, foo: -1, bar: 0, baz: 1, qux: 0},  {_id: 5, foo: -1, bar: 0, baz: -1, qux: 0},
            {_id: 6, foo: -1, bar: 0, baz: 0, qux: 0},  {_id: 7, foo: -1, bar: 0, baz: 0, qux: 0},
            {_id: 8, foo: -1, bar: 0, baz: -1, qux: 0}, {_id: 9, foo: -1, bar: 0, baz: -1, qux: 0},
            {_id: 10, foo: 0, bar: 0, baz: 0, qux: 0},  {_id: 11, foo: 0, bar: 0, baz: 0, qux: 0},
            {_id: 12, foo: 0, bar: 0, baz: 0, qux: 0},  {_id: 13, foo: 0, bar: 0, baz: 0, qux: 0},
            {_id: 14, foo: 0, bar: 0, baz: 0, qux: 0},  {_id: 15, foo: 1, bar: 1, baz: 0, qux: 1},
            {_id: 16, foo: 0, bar: 1, baz: 0, qux: 1},  {_id: 17, foo: -1, bar: 0, baz: 1, qux: 0},
            {_id: 18, foo: -1, bar: 0, baz: 1, qux: 0}, {_id: 19, foo: 0, bar: 0, baz: 0, qux: 0},
            {_id: 20, foo: -1, bar: 0, baz: 1, qux: 0}, {_id: 21, foo: -1, bar: 0, baz: 1, qux: 0},
            {_id: 22, foo: 1, bar: 1, baz: 0, qux: 1},  {_id: 23, foo: 0, bar: 0, baz: 0, qux: 0},
            {_id: 24, foo: 0, bar: 0, baz: 0, qux: 0},  {_id: 25, foo: 0, bar: 0, baz: 0, qux: 0},
            {_id: 26, foo: 0, bar: 0, baz: 0, qux: 0}
        ],
        query: {},
        proj: {
            foo: {$cmp: ["$a", "$x"]},
            bar: {$cmp: ["$a", "$b"]},
            baz: {$cmp: ["$x.y", "$v.w"]},
            qux: {$cmp: ["$a", "$nonexistent"]}
        }
    },
    //
    // Nesting torture tests.
    //
    {
        desc: "Nesting torture test 1",
        expected: computedProjectionWithBoolValues({foo: [5, 6, 7, 8, 9]}),
        query: {},
        proj: {
            foo: {
                $let: {
                    vars: {v1: {$or: ["$x.y", "$v.w"]}, vx: "$x"},
                    "in": {$and: ["$$vx.y", "$v.w"]}
                }
            }
        }
    },
    {
        desc: "Nesting torture test 2",
        expected: computedProjectionWithBoolValues({foo: [5, 6, 7, 8, 9]}),
        query: {},
        proj: {
            foo: {
                $let: {
                    vars: {v1: "$x.y"},
                    "in": {
                        $let: {
                            vars:
                                {v2: {$let: {vars: {v3: "$v.w"}, "in": {$and: ["$$v1", "$$v3"]}}}},
                            "in": "$$v2"
                        }
                    }
                }
            }
        }
    },
    {
        desc: "Nesting torture test 3",
        expected: computedProjectionWithBoolValues(
            {foo: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 18, 20, 21, 22]}),
        query: {},
        proj: {foo: {$or: ["$a", {$lt: [{$and: ["$a", "$c"]}, {$or: ["$c", "$x"]}]}, "$x"]}}
    },
    {
        desc: "Ludicrous nesting torture test with multiple computed fields",
        expected: [
            {_id: 0, foo: false, baz: false},
            {_id: 1, foo: false, baz: false},
            {_id: 2, foo: false, baz: false},
            {_id: 3, foo: 2, baz: false},
            {_id: 4, bar: 2, baz: false},
            {_id: 5, foo: true, bar: [1, 2, 3], baz: true},
            {_id: 6, foo: true, bar: 4, baz: false},
            {_id: 7, foo: true, bar: [1], baz: false},
            {_id: 8, foo: true, bar: [1, 2], baz: true},
            {_id: 9, foo: true, bar: [1, [1, 2, 3]], baz: true},
            {_id: 10, foo: false, baz: false},
            {_id: 11, foo: false, baz: false},
            {_id: 12, foo: false, baz: false},
            {_id: 13, foo: false, baz: false},
            {_id: 14, foo: false, baz: false},
            {_id: 15, foo: false, baz: false},
            {_id: 16, foo: false, baz: false},
            {_id: 17, foo: true, bar: [{z: 1}, {z: 2}], baz: false},
            {_id: 18, foo: true, bar: [3, 4, 6], baz: false},
            {_id: 19, foo: false, baz: false},
            {_id: 20, foo: true, bar: [3, 4, 6], baz: false},
            {_id: 21, foo: true, bar: [3, {z: 2}, 6], baz: false},
            {_id: 22, foo: true, baz: false},
            {_id: 23, foo: false, baz: false},
            {_id: 24, foo: false, baz: false},
            {_id: 25, foo: false, baz: false},
            {_id: 26, foo: false, baz: false}
        ],
        query: {},
        proj: {
            foo: {
                $let: {
                    vars: {
                        v1: {$or: ["$x.y", "$v.w"]},
                        v2: {$switch: {branches: [{case: "$v.w", then: 1}], default: 2}},
                        v3: "$b"
                    },
                    "in": {
                        $switch: {
                            branches: [
                                {case: {$eq: ["$x.y", 1]}, then: "$$v2"},
                                {case: {$eq: ["$x.y", 2]}, then: "$v.w"},
                                {case: {$eq: ["$$v3", 2]}, then: "$c"}
                            ],
                            default: {$or: ["$$v1", "$tf"]}
                        }
                    }
                }
            },
            bar: {
                $switch: {
                    branches: [
                        {case: {$gt: ["$x.y", 1]}, then: "$x.y"},
                        {case: {$let: {vars: {v4: "$v.w1"}, "in": "$$v4"}}, then: "$v.w2"}
                    ],
                    default: "$x.y.z"
                }
            },
            baz: {
                $let: {
                    vars: {v5: "$x.y", v6: "$v.w"},
                    "in": {
                        $cond: {
                            if: {$lt: ["$$v5", "$$v6"]},
                            then: {
                                $switch: {
                                    branches: [
                                        {case: {$eq: ["$v.w", 5]}, then: "$v.w"},
                                        {case: {$eq: ["$v.w", 4]}, then: "$x.y"}
                                    ],
                                    default: {$or: ["$$v5", "$$v6"]}
                                }
                            },
                            else: false
                        }
                    }
                }
            }
        }
    }
];

testCases.forEach(function(test) {
    if (test.expected) {
        let actual;
        assert.doesNotThrow(() => {
            actual = coll.find(test.query, test.proj).toArray();
        }, [], test);
        assert(arrayEq(actual, test.expected), Object.assign({actual: actual}, test));
    } else {
        assert(test.expectedErrorCode, test);
        const result =
            db.runCommand({find: coll.getName(), filter: test.query, projection: test.proj});
        assert.commandFailedWithCode(
            result, test.expectedErrorCode, Object.assign({result: result}, test));
    }
});

assert.commandWorked(
    db.adminCommand({configureFailPoint: "failOnPoisonedFieldLookup", mode: "off"}));
}());
