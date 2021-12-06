// Tests for $ projection operator.
// @tags: [
//   requires_getmore,
// ]
(function() {
"use strict";

load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.positional_projection;
coll.drop();

function testSingleDocument(query, projection, input, expectedOutput) {
    assert.commandWorked(coll.insert(input));
    const actualOutput = coll.findOne(query, projection);
    delete actualOutput._id;
    assert.eq(actualOutput, expectedOutput);
    assert(coll.drop());
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

testSingleDocument({'a.b': 1}, {'g.$': 1}, {a: {b: 1}}, {});

// Usually, if query predicate does not find matching array element in the input document,
// positional projection throws. But if positional projection cannot find specified path, query
// should not throw.
testSingleDocument({x: 1}, {'a.$': 1}, {x: 1}, {});

// Test that if positional projection operator cannot find specified path, existing part of it is
// returned.
testSingleDocument({x: {$gt: 0}},
                   {'a.b.c.$': 1},
                   {x: [-1, 1, 2], a: {b: {d: [1, 2, 3], f: 456}, e: 123}},
                   {a: {b: {}}});

// Test that only relevant part of specified path is extracted even in case of multiple nested
// arrays.
testSingleDocument(
    {x: 1}, {'a.b.c.$': 1}, {x: [1], a: [{b: [[[{c: 1, d: 2}]]]}]}, {a: [{b: [[[{c: 1}]]]}]});

// Test that if path specified for positional projection operator does not contain array, it is
// returned unchanged.
testSingleDocument({x: {$gt: 0}}, {'a.b.c.$': 1}, {x: [-1, 1, 2], a: {b: {c: {d: {e: 1}}}}}, {
    a: {b: {c: {d: {e: 1}}}}
});

testSingleDocument(
    {x: {$gt: 0}}, {'a.b.c.$': 1}, {x: [-1, 1, 2], a: {b: {c: 123}}}, {a: {b: {c: 123}}});

// Test that positional projection is applied to the first array on the dotted path.
// NOTE: Even though the positional projection is specified for the path 'a.b.c', it is applied to
// the first array it meets along this path. For instance, if value on a path 'a' or 'a.b' is an
// array, positional projection operator is applied only for this array.
testSingleDocument(
    {'x.y.z': 1},
    {'a.b.c.$': 1},
    {x: {y: {z: [0, 1, 2]}}, a: [{b: {c: [1, 2, 3]}}, {b: {c: [4, 5, 6]}}, {b: {c: [7, 8, 9]}}]},
    {a: [{b: {c: [4, 5, 6]}}]});

testSingleDocument(
    {'x.y.z': 1},
    {'a.b.c.$': 1},
    {x: {y: {z: [0, 1, 2]}}, a: {b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}, {c: [7, 8, 9]}]}},
    {a: {b: [{c: [4, 5, 6]}]}});

testSingleDocument({'x.y.z': 1}, {'a.b.c.$': 1}, {x: {y: {z: [0, 1, 2]}}, a: {b: {c: [1, 2, 3]}}}, {
    a: {b: {c: [2]}}
});

// Test $elemMatch in query document with positional projection.
testSingleDocument({x: {$elemMatch: {$gt: 1, $lt: 3}}},
                   {'x.$': 1},
                   {
                       x: [1, 2, 3],
                   },
                   {x: [2]});

testSingleDocument({x: {$elemMatch: {y: {$gt: 1}}}},
                   {'x.$': 1},
                   {
                       x: [{y: 1}, {y: 2}, {y: 3}],
                   },
                   {x: [{y: 2}]});

testSingleDocument({x: {$elemMatch: {a: 1, b: 2}}},
                   {'x.$': 1},
                   {x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}]},
                   {x: [{a: 1, b: 2}]});

// Test nested $elemMatch in the query document.
testSingleDocument({a: {$elemMatch: {$elemMatch: {$eq: 3}}}},
                   {"b.$": 1},
                   {a: [[1, 2], [3, 4]], b: [[5, 6], [7, 8]]},
                   {b: [[7, 8]]});

testSingleDocument({a: {$elemMatch: {p: {$elemMatch: {$eq: 2}}}}},
                   {'b.$': 1},
                   {a: [{p: [1, 2], q: [3, 4]}, {p: [5, 6], q: [7, 8]}], b: [11, 12]},
                   {b: [11]});

testSingleDocument({a: {$elemMatch: {p: {$elemMatch: {$eq: 6}}}}},
                   {'b.$': 1},
                   {a: [{p: [1, 2], q: [3, 4]}, {p: [5, 6], q: [7, 8]}], b: [11, 12]},
                   {b: [12]});

testSingleDocument({a: {$elemMatch: {q: {$elemMatch: {$eq: 3}}}}},
                   {'b.$': 1},
                   {a: [{p: [1, 2], q: [3, 4]}, {p: [5, 6], q: [7, 8]}], b: [11, 12]},
                   {b: [11]});

testSingleDocument({a: {$elemMatch: {q: {$elemMatch: {$eq: 7}}}}},
                   {'b.$': 1},
                   {a: [{p: [1, 2], q: [3, 4]}, {p: [5, 6], q: [7, 8]}], b: [11, 12]},
                   {b: [12]});

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

// Positional projection must use the index from the last child of $and operator.
testSingleDocument(
    {$and: [{a: 1}, {a: 2}, {a: 3}]}, {'b.$': 1}, {a: [1, 2, 3], b: [4, 5, 6]}, {b: [6]});

testSingleDocument(
    {$and: [{a: 3}, {a: 2}, {a: 1}]}, {'b.$': 1}, {a: [1, 2, 3], b: [4, 5, 6]}, {b: [4]});

// Positional projection must use the first matching index both for $in, and for $or
// equivalent to an $in.
testSingleDocument({a: {$in: [2, 3]}}, {'b.$': 1}, {a: [1, 2, 3], b: [4, 5, 6]}, {b: [5]});
testSingleDocument({$or: [{a: 2}, {a: 3}]}, {'b.$': 1}, {a: [1, 2, 3], b: [4, 5, 6]}, {b: [5]});

// SERVER-61839: Test out some cases involving $exists and $type where we've had bugs in the past.
testSingleDocument(
    {a: {$elemMatch: {y: {$exists: true}}}}, {"a.$": 1}, {a: [{y: 1}, {y: 2}]}, {a: [{y: 1}]});

testSingleDocument(
    {a: {$elemMatch: {y: {$type: ["array"]}}}}, {"a.$": 1}, {a: [{y: 1}, {y: []}]}, {a: [{y: []}]});

testSingleDocument({a: {$elemMatch: {y: {$type: ["array", "double"]}}}},
                   {"a.$": 1},
                   {a: [{y: 1}, {y: []}]},
                   {a: [{y: 1}]});

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

// Multiple array fields in the query document with positional projection may result in "undefined
// behaviour" according to the documentation. Here we test that at least there is no error/segfault
// for such queries.
assert.commandWorked(coll.insert({a: [1, 2, 3], b: [4, 5, 6], c: [7, 8, 9]}));
assert.doesNotThrow(() => coll.find({a: 2, b: 5}, {"c.$": 1}));
assert(coll.drop());

// Tests with invalid positional projection operator.
assert.commandWorked(coll.insert([{x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}], y: [{aa: 1}]}]));

// Positional projection cannot be used with $slice.
let err = assert.throws(() => coll.find({x: 1}, {'x.$': {'$slice': 1}}).toArray());
assert.commandFailedWithCode(err, 31271);

// Positional projection cannot be used with exclusion projection.
err = assert.throws(() => coll.find({'x.a': 2}, {'x.$': 1, y: 0}).sort({x: 1}).toArray());
assert.commandFailedWithCode(err, 31254);

// There can be only one positional projection in the query.
err = assert.throws(() => coll.find({'x.a': 1, 'y.aa': 1}, {'x.$': 1, 'y.$': 1}).toArray());
assert.commandFailedWithCode(err, 31276);

// Test queries where no array index for positional projection is recorded.
err = assert.throws(() => coll.find({}, {'x.$': 1}).toArray());
assert.commandFailedWithCode(err, 51246);

assert(coll.drop());
assert.commandWorked(coll.insert({a: [1, 2, 3], b: [4, 5, 6], c: [7, 8, 9]}));

err = assert.throws(() => coll.find({b: [4, 5, 6]}, {"c.$": 1}).toArray());
assert.commandFailedWithCode(err, 51246);

// $or with different fields, $nor and $not operators disable positional projection index
// recording for its children.
err = assert.throws(() => coll.find({$or: [{a: 1}, {b: 5}]}, {'a.$': 1}).toArray());
assert.commandFailedWithCode(err, 51246);

err = assert.throws(() => coll.find({$or: [{a: 0}, {b: 5}]}, {'a.$': 1}).toArray());
assert.commandFailedWithCode(err, 51246);

err = assert.throws(() => coll.find({$nor: [{a: -1}, {a: -2}]}, {'a.$': 1}).toArray());
assert.commandFailedWithCode(err, 51246);

assert.throws(function() {
    coll.find({'x.a': 1, 'y.aa': 1}, {'x.$': 1, 'y.$': 1}).toArray();
}, []);

err = assert.throws(function() {
    coll.find({}, {".$": 1}).toArray();
}, []);
assert.commandFailedWithCode(err, 5392900);

err = assert.throws(
    () => coll.find({a: {$not: {$not: {$elemMatch: {$eq: 1}}}}}, {'a.$': 1}).toArray());
assert.commandFailedWithCode(err, 51246);
}());
