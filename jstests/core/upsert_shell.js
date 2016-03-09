// tests to make sure that the new _id is returned after the insert in the shell
var l;
t = db.upsert1;
t.drop();

// make sure the new _id is returned when $mods are used
l = t.update({x: 1}, {$inc: {y: 1}}, true);
assert(l.getUpsertedId(), "A1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id.str, t.findOne()._id.str, "A2");

// make sure the new _id is returned on a replacement (no $mod in update)
l = t.update({x: 2}, {x: 2, y: 3}, true);
assert(l.getUpsertedId(), "B1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id.str, t.findOne({x: 2})._id.str, "B2");
assert.eq(2, t.find().count(), "B3");

// use the _id from the query for the insert
l = t.update({_id: 3}, {$set: {a: '123'}}, true);
assert(l.getUpsertedId(), "C1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, 3, "C2 - " + tojson(l));

// test with an embedded doc for the _id field
l = t.update({_id: {a: 1}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "D1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, {a: 1}, "D2 - " + tojson(l));

// test with a range query
l = t.update({_id: {$gt: 100}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "E1 - " + tojson(l));
assert.neq(l.getUpsertedId()._id, 100, "E2 - " + tojson(l));

// test with an _id query
l = t.update({_id: 1233}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "F1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, 1233, "F2 - " + tojson(l));

// test with an embedded _id query
l = t.update({_id: {a: 1, b: 2}}, {$set: {a: 123}}, true);
assert(l.getUpsertedId(), "G1 - " + tojson(l));
assert.eq(l.getUpsertedId()._id, {a: 1, b: 2}, "G2 - " + tojson(l));

// test with no _id inserted
db.no_id.drop();
db.createCollection("no_id", {autoIndexId: false});
l = db.no_id.update({foo: 1}, {$set: {a: 1}}, true);
assert(l.getUpsertedId(), "H1 - " + tojson(l));
assert(!l.hasWriteError(), "H1.5 No error expected - " + tojson(l));
assert.eq(0, db.no_id.getIndexes().length, "H2");
assert.eq(1, db.no_id.count(), "H3");
var newDoc = db.no_id.findOne();
delete newDoc["_id"];
assert.eq({foo: 1, a: 1}, newDoc, "H4");
