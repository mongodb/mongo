// @tags: [requires_fastcount, assumes_against_mongod_not_mongos]

(function() {
"use strict";

const coll = db.jstests_count;

coll.drop();
assert.commandWorked(coll.insert({i: 1}));
assert.commandWorked(coll.insert({i: 2}));
assert.eq(1, coll.find({i: 1}).count());
assert.eq(1, coll.count({i: 1}));
assert.eq(2, coll.find().count());
assert.eq(2, coll.find(undefined).count());
assert.eq(2, coll.find(null).count());
assert.eq(2, coll.count());

coll.drop();
assert.commandWorked(coll.insert({a: true, b: false}));
assert.commandWorked(coll.ensureIndex({b: 1, a: 1}));
assert.eq(1, coll.find({a: true, b: false}).count());
assert.eq(1, coll.find({b: false, a: true}).count());

coll.drop();
assert.commandWorked(coll.insert({a: true, b: false}));
assert.commandWorked(coll.ensureIndex({b: 1, a: 1, c: 1}));

assert.eq(1, coll.find({a: true, b: false}).count());
assert.eq(1, coll.find({b: false, a: true}).count());

// Make sure that invalid options passed into the shell count helper generate errors.
assert.eq(assert.throws(() => coll.count({}, {random: true})).code, 40415);

// Test that a string passed into count will be converted into a $where clause.
assert.eq(1, coll.count("this.a"));
assert.eq(0, coll.count("this.b"));
})();
