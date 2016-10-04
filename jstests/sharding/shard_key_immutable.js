/**
 * Shard key invariant:
 *
 * A document must be created with a full non-array-or-regex shard key, and the value of that shard
 * key can never change.
 *
 * To enforce this invariant, we have the following mongos rule:
 *
 * - Upserts must always contain the full shard key and must only be targeted* to the applicable
 *shard.
 *
 * and the following mongod rules:
 *
 * - Upserted shard key values must not be arrays (or regexes).
 * - If a shard key value is present in the update query, upserts must only insert documents which
 *   match this value.
 * - Updates must not modify shard keys.
 *
 * *Updates are targeted by the update query if $op-style, or the update document if
 *replacement-style.
 *
 * NOTE: The above is enough to ensure that shard keys do not change.  It is not enough to ensure
 * uniqueness of an upserted document based on the upsert query.  This is necessary due to the
 *save()
 * style operation:
 * db.coll.update({ _id : xxx }, { _id : xxx, shard : xxx, key : xxx, other : xxx }, { upsert : true
 *})
 *
 * TODO: Minimize the impact of this hole by disallowing anything but save-style upserts of this
 *form.
 * Save-style upserts of this form are not safe (duplicate _ids can be created) but the user is
 * explicitly responsible for this for the _id field.
 *
 * In addition, there is an rule where non-multi updates can only affect 0 or 1 documents.
 *
 * To enforce this, we have the following mongos rule:
 *
 * - Non-multi updates must be targeted based on an exact _id query or the full shard key.
 *
 * Test setup:
 * - replacement style updates have the multiUpdate flag set to false.
 * - $ op updates have multiUpdate flag set to true.
 */

var st = new ShardingTest({shards: 2});

st.adminCommand({enablesharding: "test"});
st.ensurePrimaryShard('test', 'shard0001');
st.adminCommand({shardcollection: "test.col0", key: {a: 1, b: 1}});
st.adminCommand({shardcollection: "test.col1", key: {'x.a': 1}});

var db = st.s.getDB('test');
var compoundColl = db.getCollection('col0');
var dotColl = db.getCollection('col1');

//
// Empty query update
//

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({}, {a: 1}, false));
var doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));
doc = compoundColl.findOne();

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({}, {a: 1, b: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({}, {a: 100, b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({}, {a: 100, b: 100, _id: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({}, {$set: {a: 1, b: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({}, {$set: {a: 100, b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({}, {$set: {a: 1, b: 1, _id: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({}, {$set: {c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

//
// Empty query upsert
//

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({}, {a: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({}, {a: 1, b: 1}, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 1, b: 1}), 'doc not upserted properly: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({}, {a: 1, b: 1, _id: 1}, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 1, b: 1}), 'doc not upserted properly: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({}, {$set: {a: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({}, {$set: {a: 1, b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeError(compoundColl.update({}, {$set: {a: 1, b: 1, _id: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({}, {$set: {c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Partial skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {a: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {a: 2}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {a: 100, b: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {a: 100, b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {a: 100, b: 100, _id: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {$set: {a: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {$set: {b: 200}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {$set: {b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {$set: {a: 100, b: 200}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {$set: {a: 100, b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100}, {$set: {a: 100, b: 100, _id: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {$set: {c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100}, {$rename: {c: 'a'}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

//
// Partial skey query upsert
//

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {a: 100}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {a: 2}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {a: 1, b: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {a: 1, b: 1, _id: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$set: {a: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$set: {b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$set: {a: 100, b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$set: {a: 100, b: 1, _id: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$set: {c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100}, {$rename: {c: 'a'}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Not prefix of skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {b: 2}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {a: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {a: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {a: 1, b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({b: 100}, {a: 100, b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {a: 1, b: 1, _id: 1}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {$set: {b: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({b: 100}, {$set: {a: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 1, b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Inspecting query and update alone is not enough to tell whether a shard key will change.
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({b: 100}, {$set: {a: 100, b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 100, b: 100, _id: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({b: 100}, {$set: {c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

//
// Not prefix of skey query upsert
//

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {b: 100}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {b: 2}, true));

doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {a: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {a: 1, b: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {a: 1, b: 1, _id: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {$set: {b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 1, b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {$set: {a: 1, b: 1, _id: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({b: 100}, {$set: {c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc upserted: ' + tojson(doc));

//
// Full skey query update
//

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100, b: 100}, {a: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100, b: 100}, {a: 100, b: 100, c: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 100}), 'doc did not change: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100, b: 100}, {a: 100, b: 100, _id: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100, b: 100}, {b: 100}, false));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {b: 100, c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 100, c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(
    compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 100, _id: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeError(compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 2, c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({a: 100, b: 100});
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {c: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'doc did not change: ' + tojson(doc));

//
// Full skey query upsert
//

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100, b: 100}, {a: 100}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {a: 100, b: 100, c: 1}, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'wrong doc: ' + tojson(doc));

// Cannot modify _id!
compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {a: 100, b: 100, _id: 100}, true));
doc = compoundColl.findOne();
assert(friendlyEqual(doc, {_id: 100, a: 100, b: 100}), 'wrong doc: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100, b: 100}, {b: 100}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {b: 100, c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc != null, 'doc was not upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 100, c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc != null, 'doc was not upserted: ' + tojson(doc));

// Can upsert with new _id
compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 100, _id: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc != null, 'doc was not upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({a: 100, b: 100}, {$set: {a: 100, b: 2, c: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({a: 100, b: 100}, {$set: {c: 1}}, true, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100, c: 1}), 'wrong doc: ' + tojson(doc));

//
// _id query update
//

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeError(compoundColl.update({_id: 1}, {a: 1}));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

// Special case for _id. This is for making save method work.
compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeOK(compoundColl.update({_id: 1}, {a: 100, b: 100}));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeError(compoundColl.update({_id: 1}, {a: 1, b: 1}));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeError(compoundColl.update({_id: 1}, {$set: {a: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeOK(compoundColl.update({_id: 1}, {$set: {a: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeError(compoundColl.update({_id: 1}, {$set: {b: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeOK(compoundColl.update({_id: 1}, {$set: {b: 100}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

compoundColl.remove({}, false);
compoundColl.insert({_id: 1, a: 100, b: 100});
assert.writeError(compoundColl.update({_id: 1}, {$set: {a: 1, b: 1}}, false, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 100, b: 100}), 'doc changed: ' + tojson(doc));

//
// _id query upsert
//

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({_id: 1}, {a: 1}, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeOK(compoundColl.update({_id: 1}, {a: 1, b: 1}, true));
doc = compoundColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {a: 1, b: 1}), 'bad doc: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({_id: 1}, {$set: {a: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({_id: 1}, {$set: {b: 1}}, true, true));
doc = compoundColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

compoundColl.remove({}, false);
assert.writeError(compoundColl.update({_id: 1}, {$set: {a: 1, b: 1}}, true, true));
assert.eq(0, compoundColl.count(), 'doc should not be inserted');

//
// Dotted query update
//

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeOK(dotColl.update({'x.a': 100}, {x: {a: 100, b: 2}}));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100, b: 2}}), 'doc did not change: ' + tojson(doc));

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.throws(function() {
    dotColl.update({'x.a': 100}, {x: {'a.z': 100}});
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.throws(function() {
    dotColl.update({'x.a': 100}, {'x.a': 100});
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.throws(function() {
    dotColl.update({'x.a': 100}, {'x.a.z': 100});
});

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {x: 100}));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {x: {b: 100}}));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {x: {a: 100, b: 2}}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100, b: 2}}), 'doc did not change: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {a: 2}}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {b: 100}}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {'x.a': 100, b: 2}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}, b: 2}), 'doc did not change: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {'a.z': 100}}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {$set: {'x.a.z': 100}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: 100}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}}), 'doc changed: ' + tojson(doc));

dotColl.remove({}, false);
dotColl.insert({x: {a: 100}});
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {'x.b': 200}}, false, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100, b: 200}}), 'doc did not change: ' + tojson(doc));

//
// Dotted query upsert
//

dotColl.remove({}, false);
assert.writeOK(dotColl.update({'x.a': 100}, {x: {a: 100, b: 2}}, true));
doc = dotColl.findOne();
assert(doc != null, 'doc was not upserted: ' + tojson(doc));

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({'x.a': 100}, {x: {'a.z': 100}}, true);
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({'x.a': 100}, {'x.a': 100}, true);
});

// Dotted field names in the resulting objects should not be allowed.
// This check currently resides in the client drivers.
dotColl.remove({}, false);
assert.throws(function() {
    dotColl.update({'x.a': 100}, {'x.a.z': 100}, true);
});

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {x: 100}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {x: {b: 100}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {x: {a: 100, b: 2}}}, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100, b: 2}}), 'bad doc: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {a: 2}}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {b: 100}}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {'x.a': 100, b: 3}}, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100}, b: 3}), 'bad doc: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {'x.a': 2}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: {'a.z': 100}}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {'x.a.z': 100}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeError(dotColl.update({'x.a': 100}, {$set: {x: 100}}, true));
doc = dotColl.findOne();
assert(doc == null, 'doc was upserted: ' + tojson(doc));

dotColl.remove({}, false);
assert.writeOK(dotColl.update({'x.a': 100}, {$set: {'x.b': 2}}, true));
doc = dotColl.findOne();
delete doc._id;
assert(friendlyEqual(doc, {x: {a: 100, b: 2}}), 'bad doc: ' + tojson(doc));

st.stop();
