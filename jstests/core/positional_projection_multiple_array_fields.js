/*
 * Test using the positional projection on documents which have multiple array fields.
 * See SERVER-6864 for details.
 *
 * Note that the user's query/filter document may only contain _ONE_ array field for positional
 * projection to work correctly.
 */
(function() {
"use strict";

const coll = db.positional_projection_multiple_array_fields;
coll.drop();

// Check that slice and positional (on different array fields) works correctly.
assert.commandWorked(coll.insert({_id: 0, a: [1, 2, 3], z: [11, 12, 13]}));
assert.eq(coll.find({z: 13}, {a: {$slice: 2}}).toArray(), [{_id: 0, a: [1, 2], z: [11, 12, 13]}]);
assert.eq(coll.find({z: 13}, {a: {$slice: 2}, "z.$": 1}).toArray(), [{_id: 0, a: [1, 2], z: [13]}]);

coll.drop();
assert.commandWorked(coll.insert({_id: 1, a: [{b: 1, c: 2}, {b: 3, c: 4}], z: [11, 12, 13]}));
assert.eq(coll.find({z: 12}, {"a.b": 1}).toArray(), [{_id: 1, a: [{"b": 1}, {"b": 3}]}]);
// The positional projection on 'z' which limits it to one element shouldn't be applied to 'a' as
// well.
assert.eq(coll.find({z: 12}, {"a.b": 1, "z.$": 1}).toArray(),
          [{_id: 1, a: [{b: 1}, {b: 3}], z: [12]}]);

// Test that a positional projection and $elemMatch fail.
let err = assert.throws(() => coll.find({z: 11}, {"z.$": 1, a: {$elemMatch: {b: 1}}}).toArray());
assert.commandFailedWithCode(err, 31255);

// Test that a positional projection which conflicts with anything fails.
err = assert.throws(() => coll.find({"a.b": 1}, {"a.$": 1, "a.b": 1}).toArray());
assert.commandFailedWithCode(err, 31249);
err = assert.throws(() => coll.find({"a.b": 1}, {"a.$": 1, "a.b": 1}).toArray());
assert.commandFailedWithCode(err, 31249);

// Multiple positional projections should fail.
err = assert.throws(() => coll.find({"a.b": 1}, {"z.$": 1, "a.$": 1}).toArray());
assert.commandFailedWithCode(err, 31277);
})();
