// @tags: [requires_fcv_73]

// Tests to ensure that duplicate fields on the same path in $or conditions are deduplicated.
// Can be merged back into upsert_fields.js in 8.0 or later once it no longer needs to be excluded
// from multiversion tests.

var res;
var coll = db[jsTestName()];
coll.drop();

// _id field has special rules

// Check things that are pretty much the same for replacement and $op style upserts

// replacement style
res = assert.commandWorked(coll.update({$or: [{_id: 1}, {_id: 1}]}, {}, true));
assert.eq(1, coll.findOne()["_id"]);

// $op style
coll.drop();
res = assert.commandWorked(coll.update({$or: [{_id: 1}, {_id: 1}]}, {$set: {a: 1}}, true));
assert.eq(1, coll.findOne()["_id"]);

coll.drop();
res = assert.commandWorked(coll.update({$or: [{_id: 1}, {_id: 1}]}, {$setOnInsert: {a: 1}}, true));
assert.eq(1, coll.findOne()["_id"]);

// Regular field extraction

// Check things that are pretty much the same for replacement and $op style upserts

// replacement style
// field extracted when replacement style
coll.drop();
res = assert.commandWorked(coll.update({$or: [{x: 1}, {x: 1}]}, {}, true));
assert.eq(undefined, coll.findOne()["x"]);

// $op style
coll.drop();
res = assert.commandWorked(coll.update({$or: [{x: 1}, {x: 1}]}, {$set: {a: 1}}, true));
assert.eq(1, coll.findOne()["x"]);

coll.drop();
res = assert.commandWorked(coll.update({$or: [{x: 1}, {x: 1}]}, {$setOnInsert: {a: 1}}, true));
assert.eq(1, coll.findOne()["x"]);
