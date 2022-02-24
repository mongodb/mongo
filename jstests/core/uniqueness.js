// @tags: [
//     # Cannot implicitly shard accessed collections because of not being able to create unique
//     # index using hashed shard key pattern.
//     cannot_create_unique_index_when_using_hashed_shard_key,
//     requires_fastcount,
//
//     # Uses index building in background
//     requires_background_index,
// ]

(function() {
"use strict";

let res;

const collNamePrefix = 'jstests_uniqueness_';
let collCount = 0;
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

// test uniqueness of _id

res = t.save({_id: 3});
assert.commandWorked(res);

// this should yield an error
res = t.insert({_id: 3});
assert.writeError(res);
assert.eq(1, t.count());

res = t.insert({_id: 4, x: 99});
assert.commandWorked(res);

// this should yield an error
res = t.update({_id: 4}, {_id: 3, x: 99});
assert.writeError(res);
assert(t.findOne({_id: 4}));

// Check for an error message when we index and there are dups
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.insert({_id: 0, a: 3}));
assert.commandWorked(t.insert({_id: 1, a: 3}));
assert.eq(2, t.find().itcount());
res = t.createIndex({a: 1}, true);
assert.commandFailed(res);
assert(res.errmsg.match(/E11000/));

// Verify that duplicate key errors follow a fixed format, including field information.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
const key = {
    _id: 1
};
const expectedMessage =
    'E11000 duplicate key error collection: ' + t + ' index: _id_ dup key: { _id: 1.0 }';
assert.commandWorked(t.insert(key));
res = t.insert(key);
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert.eq(res.nInserted, 0, tojson(res));
const writeError = res.getWriteError();
assert.includes(writeError.errmsg, expectedMessage, tojson(res));

/* Check that if we update and remove _id, it gets added back by the DB */

/* - test when object grows */
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.save({_id: 'Z'}));
assert.commandWorked(t.update({}, {k: 2}));
assert.eq('Z', t.findOne()._id, "uniqueness.js problem with adding back _id");

/* - test when doesn't grow */
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.save({_id: 'Z', k: 3}));
assert.commandWorked(t.update({}, {k: 2}));
assert.eq('Z', t.findOne()._id, "uniqueness.js problem with adding back _id (2)");
})();
