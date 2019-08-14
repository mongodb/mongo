// Tests that a rooted-$or query with each clause provably false will not return any results.
(function() {
"use strict";

const coll = db.or_always_false;
coll.drop();

assert.commandWorked(coll.insert([{}, {}, {}]));
const emptyOrError = assert.throws(() => coll.find({$or: []}).itcount());
assert.eq(emptyOrError.code, ErrorCodes.BadValue);

assert.eq(coll.find({$or: [{$alwaysFalse: 1}]}).itcount(), 0);
assert.eq(coll.find({$or: [{a: {$all: []}}]}).itcount(), 0);
assert.eq(coll.find({$or: [{$alwaysFalse: 1}, {$alwaysFalse: 1}]}).itcount(), 0);
assert.eq(coll.find({$or: [{$alwaysFalse: 1}, {a: {$all: []}}, {$alwaysFalse: 1}]}).itcount(), 0);
}());
