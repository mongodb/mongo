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

var res;

let t = db.jstests_uniqueness;

t.drop();

// test uniqueness of _id

res = t.save({_id: 3});
assert.writeOK(res);

// this should yield an error
res = t.insert({_id: 3});
assert.writeError(res);
assert.eq(1, t.count());

res = t.insert({_id: 4, x: 99});
assert.writeOK(res);

// this should yield an error
res = t.update({_id: 4}, {_id: 3, x: 99});
assert.writeError(res);
assert(t.findOne({_id: 4}));

// Check for an error message when we index and there are dups
db.jstests_uniqueness2.drop();
db.jstests_uniqueness2.insert({a: 3});
db.jstests_uniqueness2.insert({a: 3});
assert.eq(2, db.jstests_uniqueness2.count());
res = db.jstests_uniqueness2.ensureIndex({a: 1}, true);
assert.commandFailed(res);
assert(res.errmsg.match(/E11000/));

// Check for an error message when we index in the background and there are dups
db.jstests_uniqueness2.drop();
db.jstests_uniqueness2.insert({a: 3});
db.jstests_uniqueness2.insert({a: 3});
assert.eq(2, db.jstests_uniqueness2.count());
res = db.jstests_uniqueness2.ensureIndex({a: 1}, {unique: true, background: true});
assert.commandFailed(res);
assert(res.errmsg.match(/E11000/));

// Verify that duplicate key errors follow a fixed format, including field information.
const coll = db.checkDupErrorMessage;
const key = {
    _id: 1
};
const expectedMessage =
    'E11000 duplicate key error collection: ' + coll + ' index: _id_ dup key: { _id: 1.0 }';
coll.drop();
assert.commandWorked(coll.insert(key));
res = coll.insert(key);
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert.eq(res.nInserted, 0, tojson(res));
const writeError = res.getWriteError();
assert.eq(writeError.errmsg,
          expectedMessage,
          "The duplicate key error message must exactly match." + tojson(res));

/* Check that if we update and remove _id, it gets added back by the DB */

/* - test when object grows */
t.drop();
t.save({_id: 'Z'});
t.update({}, {k: 2});
assert.eq('Z', t.findOne()._id, "uniqueness.js problem with adding back _id");

/* - test when doesn't grow */
t.drop();
t.save({_id: 'Z', k: 3});
t.update({}, {k: 2});
assert.eq('Z', t.findOne()._id, "uniqueness.js problem with adding back _id (2)");
})();
