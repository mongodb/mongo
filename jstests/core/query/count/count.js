/**
 * Tests that count() in shell takes query.
 *
 * @tags: [
 *     requires_fastcount,
 *     assumes_against_mongod_not_mongos,
 *     # Reliance on the $where operator
 *     requires_scripting,
 * ]
 */

(function() {
"use strict";

const collNamePrefix = 'jstests_count_';
let collCount = 0;

let coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.insert([{_id: 1, i: 1}, {_id: 2, i: 2}]));
assert.eq(1, coll.find({i: 1}).count());
assert.eq(1, coll.count({i: 1}));
assert.eq(2, coll.find().count());
assert.eq(2, coll.find(undefined).count());
assert.eq(2, coll.find(null).count());
assert.eq(2, coll.count());

coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({b: 1, a: 1}));
assert.commandWorked(coll.insert({a: true, b: false}));
assert.eq(1, coll.find({a: true, b: false}).count());
assert.eq(1, coll.find({b: false, a: true}).count());

coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({b: 1, a: 1, c: 1}));
assert.commandWorked(coll.insert({a: true, b: false}));

assert.eq(1, coll.find({a: true, b: false}).count());
assert.eq(1, coll.find({b: false, a: true}).count());

// Make sure that invalid options passed into the shell count helper generate errors.
assert.eq(assert.throws(() => coll.count({}, {random: true})).code, 40415);

// Test that a string passed into count will be converted into a $where clause.
assert.eq(1, coll.count("this.a"));
assert.eq(0, coll.count("this.b"));
})();
