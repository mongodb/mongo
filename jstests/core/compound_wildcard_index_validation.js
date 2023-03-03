/**
 * Tests that compound wildcard indexes can be created. The specification of a compound wildcard
 * index should be validated correctly. This test also tests that CWI can be validated by validate
 * command.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 * ]
 */

(function() {
"use strict";

const coll = db.compound_wildcard_index_validation;
coll.drop();

// Tests that a subtree-indexing compound wildcard index can be created.
assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1}));
assert.commandWorked(coll.createIndex({"a.$**": 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, "b.$**": -1, c: 1}));

// Tests that an all-indexing compound wildcard index can be created only if 'wildcardProjection'
// is specified.
assert.commandWorked(coll.createIndex({a: 1, "$**": 1}, {"wildcardProjection": {a: 0}}));
assert.commandWorked(coll.createIndex({"$**": 1, a: 1}, {"wildcardProjection": {a: 0}}));
assert.commandWorked(
    coll.createIndex({a: 1, "$**": -1, b: 1}, {"wildcardProjection": {a: 0, b: 0}}));

//
// Invalid CWI specification.
//

// Tests that collision is not allowed.
assert.commandFailedWithCode(coll.createIndex({a: 1, "a.$**": 1}), 7246204);
assert.commandFailedWithCode(coll.createIndex({"a.b.c": 1, "a.b.$**": 1}), 7246204);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1, c: 1}, {"wildcardProjection": {"a.b": 1, "a.b.c": 1}}), 7246200);

// Tests that only one wildcard component is allowed.
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, "b.$**": 1}), 7246201);

// Tests that a compound wildcard index cannot contain other special type of index.
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: "text"}), 67);
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: "hashed"}), 67);
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: "2dsphere"}), 67);

// Tests that unsupported property cannot be specified.
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: 1}, {unique: true}), 67);
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: 1}, {sparse: true}), 67);
assert.commandFailedWithCode(coll.createIndex({"a.$**": 1, b: 1}, {expireAfterSeconds: 60}), 67);

// Tests that createIndex creating a compound wildcard index on all fields failed if
// 'wildcardProjection' is not specified.
assert.commandFailedWithCode(coll.createIndex({a: 1, "$**": 1}), 67);

// Tests that all compound wildcard indexes in the catalog can be validated by running validate()
// command.

// Create more wildcard indexes with various forms.
assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, str: 1}));
assert.commandWorked(coll.createIndex({"b.$**": 1, str: 1}));
assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1}));
assert.commandWorked(coll.createIndex({"$**": 1}));

// Insert documents to index.
for (let i = 0; i < 10; i++) {
    coll.insert({a: i, b: {a: i * 2, c: i * i}, str: 'aa'});
}

assert.commandWorked(coll.validate({full: true}));
})();
