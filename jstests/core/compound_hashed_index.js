/**
 * Test to verify the behaviour of compound hashed indexes.
 */
(function() {
"use strict";

const coll = db.compound_hashed_index;
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

// Verify inserts and updates work when there are no arrays along path.
assert.commandWorked(coll.insert({field1: {field2: {0: {otherField: []}}}}));
assert.commandWorked(coll.insert({field1: {field2: {0: {field4: 1}}}}));
assert.commandWorked(coll.update({}, {field1: {field2: {0: {field4: 1}}}}));
})();