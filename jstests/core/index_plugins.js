// Test creation of compound indexes with special index types.
(function() {
"use strict";
const coll = db.index_plugins;
coll.drop();

// Test building special index types on a single field.

assert.commandWorked(coll.createIndex({a: "hashed"}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: "2d"}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: "2dsphere"}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: "text"}));
coll.dropIndexes();

// Test compounding special index types with an ascending index.

assert.commandWorked(coll.createIndex({a: "2dsphere", b: 1}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1, b: "2dsphere"}));
coll.dropIndexes();

assert.commandWorked(coll.createIndex({a: "text", b: 1}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1, b: "text"}));
coll.dropIndexes();

assert.commandWorked(coll.createIndex({a: "2d", b: 1}));
assert.commandWorked(coll.createIndex({a: "hashed", b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: "hashed"}));

coll.dropIndexes();
assert.commandFailed(coll.createIndex({a: 1, b: "2d"}));  // unsupported

// Test compound index where multiple fields have same special index type.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: "2dsphere", b: "2dsphere"}));
assert.commandWorked(coll.createIndex({a: "text", b: "text"}));
// Unsupported.
assert.commandFailed(coll.createIndex({a: "2d", b: "2d"}));
assert.commandFailedWithCode(coll.createIndex({a: "hashed", b: "hashed"}), 31303);
assert.commandFailedWithCode(coll.createIndex({c: 1, a: "hashed", b: "hashed"}), 31303);

// Test compounding different special index types with each other.
const incompatableIndexTypes = ["2d", "2dsphere", "hashed", "text"];
for (let indexType1 of incompatableIndexTypes) {
    for (let indexType2 of incompatableIndexTypes) {
        if (indexType1 == indexType2) {
            continue;
        }
        assert.commandFailedWithCode(coll.createIndex({a: indexType1, b: indexType2}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({a: indexType1, b: indexType2, c: 1}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({c: -1, a: indexType1, b: indexType2}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({a: indexType1, c: 1, b: indexType2}),
                                     ErrorCodes.CannotCreateIndex);
    }
    assert.commandFailedWithCode(coll.createIndex({"$**": 1, b: indexType1}),
                                 ErrorCodes.CannotCreateIndex);
}
})();
