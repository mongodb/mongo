// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection, requires_fastcount]

// tests to make sure that the new _id is returned after the insert in the shell
var l;
t = db.upsert1;
t.drop();

// make sure the new _id is returned when $mods are used
l = t.update({x: 1}, {$inc: {y: 1}}, true);
assert(l.getUpsertedId(), "A1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id.str, t.findOne()._id.str, "A2");
assert.eq(l._id.str, t.findOne()._id.str, "A2");

// make sure the new _id is returned on a replacement (no $mod in update)
l = t.update({x: 2}, {x: 2, y: 3}, true);
assert(l.getUpsertedId(), "B1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id.str, t.findOne({x: 2})._id.str, "B2");
assert.eq(l._id.str, t.findOne({x: 2})._id.str, "B2");
assert.eq(2, t.find().count(), "B3");

// make sure that an upsert update that only updates doesn't return and _id
l = t.update({x: 2}, {x: 2, y: 4}, true);
assert(l.getUpsertedId() === null);
assert(l._id === undefined);

// use the _id from the query for the insert
l = t.update({_id: 3}, {$set: {a: '123'}}, true);
assert(l.getUpsertedId(), "C1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, 3, "C2 - " + tojson(l));
assert.eq(l._id, 3, "C2 - " + tojson(l));

// test with an embedded doc for the _id field
l = t.update({_id: {a: 1}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "D1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, {a: 1}, "D2 - " + tojson(l));
assert.eq(l._id, {a: 1}, "D2 - " + tojson(l));

// test with a range query
l = t.update({_id: {$gt: 100}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "E1 - " + tojson(l));
assert.neq(l.getUpsertedId()._id, 100, "E2 - " + tojson(l));
assert.neq(l._id, 100, "E2 - " + tojson(l));

// test with an _id query
l = t.update({_id: 1233}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "F1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, 1233, "F2 - " + tojson(l));
assert.eq(l._id, 1233, "F2 - " + tojson(l));

// test with an embedded _id query
l = t.update({_id: {a: 1, b: 2}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "G1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, {a: 1, b: 2}, "G2 - " + tojson(l));
assert.eq(l._id, {a: 1, b: 2}, "G2 - " + tojson(l));
