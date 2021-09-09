/**
 * Tests inserting various _id values and duplicates on a collection clustered by _id.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   requires_fcv_51,
 *   # TODO: (SERVER-59199) Support appending large RecordIds to KeyStrings
 *   requires_wiredtiger,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const collName = 'system.buckets.clustered_index_types';
const coll = db[collName];
coll.drop();

assert.commandWorked(db.createCollection(collName, {clusteredIndex: true}));

// Expect that duplicates are rejected.
let oid = new ObjectId();
assert.commandWorked(coll.insert({_id: oid}));
assert.commandFailedWithCode(coll.insert({_id: oid}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: oid}).itcount());

// Updates should work.
assert.commandWorked(coll.update({_id: oid}, {a: 1}));
assert.eq(1, coll.find({_id: oid}).itcount());

assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.eq(1, coll.find({_id: 0}).itcount());
assert.commandWorked(coll.insert({_id: "", a: 2}));
assert.eq(1, coll.find({_id: ""}).itcount());
assert.commandWorked(coll.insert({_id: NumberLong("9223372036854775807"), a: 3}));
assert.eq(1, coll.find({_id: NumberLong("9223372036854775807")}).itcount());
assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, a: 4}));
assert.eq(1, coll.find({_id: {a: 1, b: 1}}).itcount());
assert.commandWorked(coll.insert({_id: {a: {b: 1}, c: 1}, a: 5}));
assert.commandWorked(coll.insert({_id: -1, a: 6}));
assert.eq(1, coll.find({_id: -1}).itcount());
assert.commandWorked(coll.insert({_id: "123456789012", a: 7}));
assert.eq(1, coll.find({_id: "123456789012"}).itcount());
assert.commandWorked(coll.insert({a: 8}));
assert.eq(1, coll.find({a: 8}).itcount());
assert.commandWorked(coll.insert({_id: null, a: 9}));
assert.eq(1, coll.find({_id: null}).itcount());
assert.commandWorked(coll.insert({_id: 'x'.repeat(100), a: 10}));
assert.commandWorked(coll.insert({}));

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.dropIndex({a: 1}));

// This key is too large.
assert.commandFailedWithCode(coll.insert({_id: 'x'.repeat(8 * 1024 * 1024), a: 11}), 5894900);
// Large key but within the upper bound
assert.commandWorked(coll.insert({_id: 'x'.repeat(3 * 1024 * 1024), a: 12}));
// Can build a secondary index with a 3MB RecordId doc.
assert.commandWorked(coll.createIndex({a: 1}));

// Look up using the secondary index on {a: 1}
assert.eq(1, coll.find({a: null}).itcount());
assert.eq(0, coll.find({a: 0}).itcount());
assert.eq(2, coll.find({a: 1}).itcount());
assert.eq(1, coll.find({a: 2}).itcount());
assert.eq(1, coll.find({a: 8}).itcount());
assert.eq(1, coll.find({a: 9}).itcount());
assert.eq(null, coll.findOne({a: 9})['_id']);
assert.eq(1, coll.find({a: 10}).itcount());
assert.eq(100, coll.findOne({a: 10})['_id'].length);
assert.eq(1, coll.find({a: 12}).itcount());
assert.eq(3 * 1024 * 1024, coll.findOne({a: 12})['_id'].length);

// No support for numeric type differentiation.
assert.commandWorked(coll.insert({_id: 42.0}));
assert.commandFailedWithCode(coll.insert({_id: 42}), ErrorCodes.DuplicateKey);
assert.commandFailedWithCode(coll.insert({_id: NumberLong("42")}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: 42.0}).itcount());
assert.eq(1, coll.find({_id: 42}).itcount());
assert.eq(1, coll.find({_id: NumberLong("42")}).itcount());
})();
