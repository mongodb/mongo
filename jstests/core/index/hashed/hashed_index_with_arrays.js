/**
 * Test to verify the behaviour of compound hashed indexes.
 */
(function() {
"use strict";

const coll = db.hashed_index_with_arrays;
coll.drop();

for (let i = 0; i < 20; i++) {
    assert.commandWorked(
        coll.insert({a: i, b: {subObj: "string_" + (i % 13)}, c: NumberInt(i % 10)}));
}

// Creation of compound hashed indexes work.
assert.commandWorked(coll.createIndex({a: 1, b: "hashed", c: 1}));
assert.commandWorked(coll.createIndex({a: "hashed", c: -1}));
assert.commandWorked(coll.createIndex({b: "hashed", a: -1, c: 1}));

// None of the index fields can be an array.
assert.commandFailedWithCode(coll.insert({a: []}), 16766);
assert.commandFailedWithCode(coll.insert({b: []}), 16766);
assert.commandFailedWithCode(coll.insert({c: []}), 16766);

// Test that having arrays along the path of the index is not allowed.
assert.commandWorked(coll.createIndex({"field1.field2.0.field4": "hashed", "field2.0.field4": 1}));

// Hashed field path cannot have arrays.
assert.commandFailedWithCode(coll.insert({field1: []}), 16766);
assert.commandFailedWithCode(coll.insert({field1: {field2: []}}), 16766);
assert.commandFailedWithCode(coll.insert({field1: {field2: {0: []}}}), 16766);
assert.commandFailedWithCode(coll.insert({field1: [{field2: {0: []}}]}), 16766);
assert.commandFailedWithCode(coll.insert({field1: {field2: {0: {field4: []}}}}), 16766);

// Range field path cannot have arrays.
assert.commandFailedWithCode(coll.insert({field2: []}), 16766);
assert.commandFailedWithCode(coll.insert({field2: {0: [0]}}), 16766);

// Verify that updates gets rejected when a document is modified to contain array along the path.
assert.commandFailedWithCode(coll.update({}, {field2: []}), 16766);
assert.commandFailedWithCode(coll.update({}, {field2: {0: {field4: []}}}), 16766);
assert.commandFailedWithCode(coll.update({}, {field1: []}), 16766);
assert.commandFailedWithCode(coll.update({_id: "missing"}, {field1: []}, {upsert: true}), [16766]);

// Verify inserts and updates work when there are no arrays along path.
assert.commandWorked(coll.insert({field1: {field2: {0: {otherField: []}}}}));
assert.commandWorked(coll.insert({field1: {field2: {0: {field4: 1}}}}));
assert.commandWorked(coll.update({}, {field1: {field2: {0: {field4: 1}}}}));
assert.commandWorked(
    coll.update({_id: "missing"}, {field1: {field2: {0: {field4: 1}}}}, {upsert: true}));

/**
 * Tests for sparse indexes.
 */
// Creation of compound hashed indexes work.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({"a.b": 1, c: "hashed", d: -1}, {sparse: true}));
assert.commandWorked(coll.createIndex({"a.c": "hashed", d: -1}, {sparse: true}));
assert.commandWorked(coll.createIndex({b: "hashed", d: -1, c: 1}, {sparse: true}));

// Any arrays not allowed for sparse index.
assert.commandFailedWithCode(coll.insert({b: []}), 16766);
assert.commandFailedWithCode(coll.insert({c: [1]}), 16766);
assert.commandFailedWithCode(coll.insert({a: []}), 16766);

/**
 * Tests for partial indexes.
 */
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(
    coll.createIndex({a: "hashed", b: 1}, {partialFilterExpression: {b: {$gt: 5}}}));
assert.commandFailedWithCode(coll.insert({a: [1], b: 6}), 16766);

// Array insertion allowed when the document doesn't match the partial filter predication.
assert.commandWorked(coll.insert({a: [1], b: 1}));
})();
