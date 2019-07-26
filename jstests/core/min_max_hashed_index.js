/*
 * Check that min() and max() work with a hashed index.
 */
(function() {
"use strict";

const coll = db.min_max_hashed_index;
coll.drop();
assert.commandWorked(coll.insert({a: "test"}));
assert.commandWorked(coll.createIndex({a: 1}));
const minWithNormalIndex = coll.find({}, {_id: 0}).min({a: -Infinity}).hint({a: 1}).toArray();
assert.eq(minWithNormalIndex, [{a: "test"}]);

assert.commandWorked(coll.createIndex({a: "hashed"}));
const minWithHashedIndex =
    coll.find({}, {_id: 0}).min({a: -Infinity}).hint({a: "hashed"}).toArray();
assert.eq(minWithHashedIndex, [{a: "test"}]);
})();
