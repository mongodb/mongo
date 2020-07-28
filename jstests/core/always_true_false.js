// Tests $alwaysTrue and $alwaysFalse behavior for match expressions.
(function() {
"use strict";

const coll = db.always_true_false;
coll.drop();

assert.commandWorked(
    coll.insert([{a: []}, {a: false}, {a: null}, {}, {a: false, b: 2}, {a: false, b: 1}]));

// Check alwaysFalse.
assert.eq(0, coll.find({$alwaysFalse: 1}).itcount());
assert.eq(0, coll.find({$alwaysFalse: 1, a: false}).itcount());
assert.eq(0, coll.find({$alwaysFalse: 1, b: 1}).itcount());

// Check alwaysFalse with $and, $or.
assert.eq(1, coll.find({$or: [{b: 1}, {$alwaysFalse: 1}]}).itcount());
assert.eq(0, coll.find({$or: [{$alwaysFalse: 1}]}).itcount());
assert.eq(0, coll.find({$or: [{$alwaysFalse: 1}, {$alwaysFalse: 1}]}).itcount());
assert.eq(0, coll.find({$or: [{$alwaysFalse: 1}, {a: {$all: []}}, {$alwaysFalse: 1}]}).itcount());
assert.eq(0, coll.find({$and: [{b: 1}, {$alwaysFalse: 1}]}).itcount());
assert.eq(0, coll.find({$and: [{a: false}, {$alwaysFalse: 1}, {$alwaysFalse: 1}]}).itcount());

// Check alwaysTrue.
assert.eq(6, coll.find({$alwaysTrue: 1}).itcount());
assert.eq(3, coll.find({$alwaysTrue: 1, a: false}).itcount());
assert.eq(1, coll.find({$alwaysTrue: 1, b: 1}).itcount());

// Check alwaysTrue with $and, $or.
assert.eq(3, coll.find({$and: [{a: false}, {$alwaysTrue: 1}, {$alwaysTrue: 1}]}).itcount());
assert.eq(0, coll.find({$and: [{a: false}, {$alwaysTrue: 1}, {$alwaysFalse: 1}]}).itcount());
assert.eq(6, coll.find({$or: [{b: 1}, {$alwaysTrue: 1}]}).itcount());
assert.eq(6, coll.find({$or: [{b: 1}, {$alwaysFalse: 1}, {$alwaysTrue: 1}]}).itcount());

assert(coll.drop());

// Check that a rooted-$or query with each clause false will not return any results.
assert.commandWorked(coll.insert([{}, {}, {}]));
const emptyOrError = assert.throws(() => coll.find({$or: []}).itcount());
assert.eq(emptyOrError.code, ErrorCodes.BadValue);

assert.eq(coll.find({$or: [{$alwaysFalse: 1}]}).itcount(), 0);
assert.eq(coll.find({$or: [{a: {$all: []}}]}).itcount(), 0);
assert.eq(coll.find({$or: [{$alwaysFalse: 1}, {$alwaysFalse: 1}]}).itcount(), 0);
assert.eq(coll.find({$or: [{$alwaysFalse: 1}, {a: {$all: []}}, {$alwaysFalse: 1}]}).itcount(), 0);

// Check failure cases.
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {$alwaysTrue: 0}}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {$alwaysFalse: 0}}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$alwaysFalse: 1}}}),
                             ErrorCodes.BadValue);
}());
