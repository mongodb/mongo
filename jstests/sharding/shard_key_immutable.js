/**
 * This file test that updates on a sharded collection obeys the shard key invariants.
 * The invariant boils down into these rules:
 *
 * 1. If the update is a replacement style update or an upsert, the full shard key must
 *    be present in the update object.
 * 2. If the update object contains a shard key, the value in the query object should
 *    be equal.
 * 3. If the update object contains a shard key but is not present in the query, then
 *    the matching documents should have the same value.
 *
 * Test setup:
 * - replacement style updates have the multiUpdate flag set to false.
 * - $ op updates have multiUpdate flag set to true.
 */

var st = new ShardingTest({ shards: 2 });

st.adminCommand({ enablesharding: "test" });
st.adminCommand({ shardcollection: "test.col0", key: { a: 1, b: 1 }});
st.adminCommand({ shardcollection: "test.col1", key: { 'x.a': 1 }});

var db = st.s.getDB('test');
var compoundColl = db.getCollection('col0');
var dotColl = db.getCollection('col1');

//
// Empty query update
//

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { a: 1 }, false);
var gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
var doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
doc = compoundColl.findOne();

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { a: 1, b: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { a: 100, b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { a: 100, b: 100, _id: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { $set: { a: 1, b: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { $set: { a: 100, b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

// Cannot modify _id
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { $set: { a: 1, b: 1, _id: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({}, { $set: { c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'doc did not change: ' + tojson(doc));

//
// Empty query upsert
//

compoundColl.remove({}, false);
compoundColl.update({}, { a: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.update({}, { a: 1, b: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({}, { a: 1, b: 1, _id: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.update({}, { $set: { a: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({}, { $set: { a: 1, b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({}, { $set: { a: 1, b: 1, _id: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({}, { $set: { c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Partial skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { a: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { a: 2 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { a: 100, b: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { a: 100, b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { a: 100, b: 100, _id: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { a: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { b: 200 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { a: 100, b: 200 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { a: 100, b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { a: 100, b: 100, _id: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $set: { c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'doc did not change: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100 }, { $rename: { c: 'a' }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

//
// Partial skey query upsert
//

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { a: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { a: 2 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle), true);
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { a: 1, b: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { a: 1, b: 1, _id: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $set: { a: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $set: { b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $set: { a: 100, b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $set: { a: 100, b: 1, _id: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $set: { c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100 }, { $rename: { c: 'a' }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Not prefix of skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { b: 2 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { a: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { a: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { a: 1, b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { a: 100, b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { a: 1, b: 1, _id: 1 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { b: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { a: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { a: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { a: 1, b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
/*
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { a: 100, b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { a: 100, b: 100, _id: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ b: 100 }, { $set: { c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'doc did not change: ' + tojson(doc));

//
// Not prefix of skey query upsert
//

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { b: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { b: 2 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { a: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { a: 1, b: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { a: 1, b: 1, _id: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { $set: { b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { $set: { a: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { $set: { a: 1, b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { $set: { a: 1, b: 1, _id: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ b: 100 }, { $set: { c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Full skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { a: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { a: 100, b: 100, c: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 100 }), 'doc did not change: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { a: 100, b: 100, _id: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { b: 100 }, false);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { $set: { b: 100, c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 100, c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 100, _id: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 2, c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({ a: 100, b: 100 });
compoundColl.update({ a: 100, b: 100 }, { $set: { c: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
doc = compoundColl.findOne();
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'doc did not change: ' + tojson(doc));

//
// Full skey query upsert
//

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { a: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { a: 100, b: 100, c: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'wrong doc: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { a: 100, b: 100, _id: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(friendlyEqual(doc, { _id: 100, a: 100, b: 100 }), 'wrong doc: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { b: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { $set: { b: 100, c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 100, c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 100, _id: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { $set: { a: 100, b: 2, c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ a: 100, b: 100 }, { $set: { c: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100, c: 1 }), 'wrong doc: ' + tojson(doc));

//
// _id query update
//

compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { a: 1 });
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

// Special case for _id. This is for making save method work.
compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { a: 100, b: 100 });
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { a: 1, b: 1 });
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { $set: { a: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { $set: { a: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { $set: { b: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { $set: { b: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));
*/

compoundColl.remove({}, false);
compoundColl.insert({ _id: 1, a: 100, b: 100 });
compoundColl.update({ _id: 1 }, { $set: { a: 1, b: 1 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 100, b: 100 }), 'doc changed: ' + tojson(doc));

//
// _id query upsert
//

compoundColl.remove({}, false);
compoundColl.update({ _id: 1 }, { a: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ _id: 1 }, { a: 1, b: 1 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 1, b: 1 }), 'bad doc: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ _id: 1 }, { $set: { a: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.update({ _id: 1 }, { $set: { b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

/*
compoundColl.remove({}, false);
compoundColl.update({ _id: 1 }, { $set: { a: 1, b: 1 }}, true, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { a: 1, b: 1 }), 'bad doc: ' + tojson(doc));
*/

//
// Dotted query update
//

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { x: { a: 100, b: 2 }});
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { x: { 'a.z': 100 }});
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { 'x.a': 100 });
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { 'x.a.z': 100 });
});

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { x: 100 });
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { x: { b: 100 }});
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { x: { a: 100, b: 2 }}}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100, b: 2 }}), 'doc did not change: ' + tojson(doc));
*/

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { x: { a: 2 }}}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { x: { b: 100 }}}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { 'x.a': 100, b: 2 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }, b: 2 }), 'doc did not change: ' + tojson(doc));
*/

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { x: { 'a.z': 100 }}}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { 'x.a.z': 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { x: 100 }}, false, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100 }}), 'doc changed: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.insert({ x: { a: 100 }});
dotColl.update({ 'x.a': 100 }, { $set: { 'x.b': 200 }}, false, true);
assert(gle.err == null, 'gleObj: ' + tojson(gle));
gle = db.runCommand({ getLastError: 1 });
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, { x: { a: 100, b: 200 }}), 'doc did not change: ' + tojson(doc));
*/

//
// Dotted query upsert
//

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { x: { a: 100, b: 2 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { x: { 'a.z': 100 }}, true);
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { 'x.a': 100 }, true);
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({ 'x.a': 100 }, { 'x.a.z': 100 }, true);
});

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { x: 100 }, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { x: { b: 100 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: { a: 100, b: 2 }}}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(friendlyEqual(doc, { x: { a: 100, 2: 3 }}), 'bad doc: ' + tojson(doc));
*/

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: { a: 2 }}}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: { b: 100 }}}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { 'x.a': 100, b: 3 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err == null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(friendlyEqual(doc, { x: { a: 100 }, b: 3 }), 'bad doc: ' + tojson(doc));
*/

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { 'x.a': 2 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: { b: 100 }}}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: { 'a.z': 100 }}}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { 'x.a.z': 100 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { x: 100 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

/*
dotColl.remove({}, false);
dotColl.update({ 'x.a': 100 }, { $set: { 'x.b': 2 }}, true);
gle = db.runCommand({ getLastError: 1 });
assert(gle.err != null, 'gleObj: ' + tojson(gle));
doc = compoundColl.findOne();
assert(friendlyEqual(doc, { x: { a: 100, b: 2 }}), 'bad doc: ' + tojson(doc));
*/

st.stop();

