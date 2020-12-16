/**
 * Test using the positional projection on documents which have multiple array fields.
 * See SERVER-6864 for details.
 *
 * Note that the user's query/filter document may only contain _ONE_ array field for positional
 * projection to work correctly.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For documentEq.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.positional_projection_multiple_array_fields;
coll.drop();

function testSingleDocument(query, projection, expected) {
    assert(documentEq(coll.findOne(query, projection), expected));
}

// Check that slice and positional (on different array fields) works correctly.
assert.commandWorked(coll.insert({_id: 0, a: [1, 2, 3], z: [11, 12, 13]}));
testSingleDocument({z: 13}, {a: {$slice: 2}}, {_id: 0, a: [1, 2], z: [11, 12, 13]});
testSingleDocument({z: 13}, {a: {$slice: 2}, "z.$": 1}, {_id: 0, a: [1, 2], z: [13]});
assert(coll.drop());

coll.insert({_id: 0, importing: [{foo: "a"}, {foo: "b"}], "jobs": [{num: 1}, {num: 2}, {num: 3}]});
testSingleDocument({"importing.foo": "b"},
                   {jobs: {'$slice': 2}, 'importing.$': 1},
                   {_id: 0, importing: [{foo: "b"}], jobs: [{num: 1}, {num: 2}]});
testSingleDocument({"importing.foo": "b"},
                   {jobs: {'$slice': -1}, 'importing.$': 1},
                   {_id: 0, importing: [{foo: "b"}], jobs: [{num: 3}]});

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 1, a: [{b: 1, c: 2}, {b: 3, c: 4}], z: [11, 12, 13]}));
testSingleDocument({z: 12}, {"a.b": 1}, {_id: 1, a: [{"b": 1}, {"b": 3}]});

// The positional projection on 'z' which limits it to one element shouldn't be applied to 'a' as
// well.
testSingleDocument({z: 12}, {"a.b": 1, "z.$": 1}, {_id: 1, a: [{b: 1}, {b: 3}], z: [12]});

// Test that the positional projection can be applied to a "parallel" array.
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 1, a: [1, 2, 3], b: ["one", "two", "three"]}));
testSingleDocument({a: 2}, {"b.$": 1}, {_id: 1, b: ["two"]});

// Similar test, but try a parallel array which is on a dotted path.
assert.commandWorked(coll.insert({_id: 2, a: {b: [1, 2, 3]}, c: {d: ["one", "two", "three"]}}));
testSingleDocument({"a.b": 2}, {"c.d.$": 1}, {_id: 2, c: {d: ["two"]}});

// Attempting to apply it to a parallel array which is smaller.
assert.commandWorked(coll.insert({_id: 3, a: [4, 5, 6], b: ["four", "five"]}));
let err = assert.throws(() => coll.find({a: 6}, {"b.$": 1}).toArray());
assert.commandFailedWithCode(err, 51247);

// Test that a positional projection and $elemMatch fail.
err = assert.throws(() => coll.find({z: 11}, {"z.$": 1, a: {$elemMatch: {b: 1}}}).toArray());
assert.commandFailedWithCode(err, 31255);

// Test that a positional projection which conflicts with anything fails.
err = assert.throws(() => coll.find({"a.b": 1}, {"a.$": 1, "a.b": 1}).toArray());
assert.commandFailedWithCode(err, 31249);
err = assert.throws(() => coll.find({"a.b": 1}, {"a.$": 1, "a.b": 1}).toArray());
assert.commandFailedWithCode(err, 31249);

// Multiple positional projections should fail.
err = assert.throws(() => coll.find({"a.b": 1}, {"z.$": 1, "a.$": 1}).toArray());
assert.commandFailedWithCode(err, 31276);
})();
