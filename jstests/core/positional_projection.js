// Tests for $ projection operator.
// @tags: [
//   requires_getmore,
//   sbe_incompatible,
//   requires_fcv_49
// ]
(function() {
"use strict";

const coll = db.positional_projection;
coll.drop();

function testSingleDocument(query, projection, input, expectedOutput, dropCollection = true) {
    assert.commandWorked(coll.insert(input));
    const actualOutput = coll.findOne(query, projection);
    delete actualOutput._id;
    assert.eq(actualOutput, expectedOutput);
    if (dropCollection) {
        assert(coll.drop());
    }
}

// Single object match.
testSingleDocument({'x.a': 2},
                   {'x.$': 1},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {x: [{a: 2, c: 3}]});

testSingleDocument({'x.a': 1},
                   {'x.$': 1},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {x: [{a: 1, b: 2}]});

testSingleDocument({'x.a': 1}, {'x.$': 1}, {x: [{a: 1, b: 3}, {a: -6, c: 3}]}, {x: [{a: 1, b: 3}]});

testSingleDocument({'y.dd': 5},
                   {'y.$': 1},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {y: [{aa: 1, dd: 5}]});

// $elemMatch with positional projection.
testSingleDocument({x: {$elemMatch: {a: 1, b: 2}}},
                   {'x.$': 1},
                   {x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}]},
                   {x: [{a: 1, b: 2}]});

// Nested paths.
testSingleDocument(
    {'x.y.a': 1}, {'x.y.$': 1}, {x: {y: [{a: 1, b: 1}, {a: 1, b: 2}]}}, {x: {y: [{a: 1, b: 1}]}});

// Positional projection on non-existent field.
testSingleDocument({},
                   {'g.$': 1},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {});

// Regular index test.
assert.commandWorked(coll.createIndex({'y.d': 1}));
testSingleDocument({'y.d': 4},
                   {'y.$': 1},
                   {x: [{a: 1, b: 2}, {a: 3, b: 4}], y: [{c: 1, d: 2}, {c: 3, d: 4}]},
                   {y: [{c: 3, d: 4}]});

// Covered index test.
assert.commandWorked(coll.createIndex({covered: 1}));
testSingleDocument({'covered.dd': 5},
                   {'covered.$': 1},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       covered: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {covered: [{aa: 1, dd: 5}]});

// Tests involving getMore. Test the $-positional operator across multiple batches.
assert.commandWorked(coll.insert([
    {
        _id: 0,
        group: 3,
        x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
        y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
    },
    {_id: 1, group: 3, x: [{a: 1, b: 3}, {a: -6, c: 3}]},
]));

let it = coll.find({'x.b': 2}, {'x.$': 1}).sort({_id: 1}).batchSize(1);
while (it.hasNext()) {
    const currentDocument = it.next();
    assert.eq(2, currentDocument.x[0].b);
}

assert(coll.drop());

// Tests with invalid positional projection operator.
assert.commandWorked(coll.insert([{x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}], y: [{aa: 1}]}]));
let err = assert.throws(() => coll.find({x: 1}, {'x.$': {'$slice': 1}}).toArray());
assert.commandFailedWithCode(err, 31271);

assert.throws(function() {
    coll.find({'x.a': 2}, {'x.$': 1, y: 0}).sort({x: 1}).toArray();
}, []);

assert.throws(function() {
    coll.find({'x.a': 1, 'y.aa': 1}, {'x.$': 1, 'y.$': 1}).toArray();
}, []);

err = assert.throws(function() {
    coll.find({}, {".$": 1}).toArray();
}, []);
assert.commandFailedWithCode(err, 5392900);
}());
