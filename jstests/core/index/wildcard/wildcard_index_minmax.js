/**
 * Tests that min/max is not supported for wildcard index.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.

const coll = db.wildcard_index_minmax;
coll.drop();

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 2}));

assert.commandWorked(coll.createIndex({"$**": 1}));
assert.commandWorked(coll.createIndex({"a": 1}));

// Throws error for $** index min.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), min: {"a": 0.5}, hint: {"$**": 1}}), 51174);

// Throws error for $** index max.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), max: {"a": 1.5}, hint: {"$**": 1}}), 51174);

// Throws error for $** index min/max.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), min: {"a": 0.5}, max: {"a": 1.5}, hint: {"$**": 1}}),
    51174);

// Throws error for $** index min with filter of a different value.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 2}, min: {"a": 1}, hint: {"$**": 1}}),
    51174);

// Throws error for $** index max with filter of a different value.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 1}, max: {"a": 1.5}, hint: {"$**": 1}}),
    51174);

// Throws error for $** index min and max with filter of a different value.
assert.commandFailedWithCode(db.runCommand({
    find: coll.getName(),
    filter: {"a": 1},
    min: {"a": 0.5},
    max: {"a": 1.5},
    hint: {"$**": 1}
}),
                             51174);

// Throws error for $** index min with filter of the same value.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 1}, min: {"a": 1}, hint: {"$**": 1}}),
    51174);

// Throws error for $** index max with filter of the same value.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 1}, max: {"a": 1}, hint: {"$**": 1}}),
    51174);

// Throws error for $** index min and max with filter of the same value.
assert.commandFailedWithCode(
    db.runCommand(
        {find: coll.getName(), filter: {"a": 1}, min: {"a": 1}, max: {"a": 1}, hint: {"$**": 1}}),
    51174);

// $** index does not interfere with valid min/max.
assertArrayEq(coll.find({}, {_id: 0}).min({"a": 0.5}).max({"a": 1.5}).hint({a: 1}).toArray(),
              [{a: 1, b: 1}, {a: 1, b: 2}]);
})();
