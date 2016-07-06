/**
 * These tests verify that the oplog entries are created correctly for updates
 *
 * Do not add more tests here but instead add C++ unit tests in db/ops/modifier*_test files
 *
 */

"use strict";
var replTest = new ReplSetTest({nodes: 1, oplogSize: 2, nodeOptions: {smallfiles: ""}});
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();
var coll = master.getDB("o").fake;
var cdb = coll.getDB();

var assertLastOplog = function(o, o2, msg) {
    var last = master.getDB("local").oplog.rs.find().limit(1).sort({$natural: -1}).next();

    assert.eq(last.ns, coll.getFullName(), "ns bad : " + msg);
    assert.docEq(last.o, o, "o bad : " + msg);
    if (o2)
        assert.docEq(last.o2, o2, "o2 bad : " + msg);
    return last.ts;
};

// set things up.
coll.save({_id: 1});
assertLastOplog({_id: 1}, null, "save -- setup ");

/**
 * The first ones are from the old updatetests which tested the internal impl using a modSet
 */

var msg = "IncRewriteExistingField: $inc $set";
coll.save({_id: 1, a: 2});
assertLastOplog({_id: 1, a: 2}, {_id: 1}, "save " + msg);
var res = assert.writeOK(coll.update({}, {$inc: {a: 1}, $set: {b: 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: 3, b: 2}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 3, b: 2}}, {_id: 1}, msg);

var msg = "IncRewriteNonExistingField: $inc $set";
coll.save({_id: 1, c: 0});
assertLastOplog({_id: 1, c: 0}, {_id: 1}, "save " + msg);
res = assert.writeOK(coll.update({}, {$inc: {a: 1}, $set: {b: 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, c: 0, a: 1, b: 2}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 1, b: 2}}, {_id: 1}, msg);

var msg = "TwoNestedPulls: two $pull";
coll.save({_id: 1, a: {b: [1, 2], c: [1, 2]}});
assertLastOplog({_id: 1, a: {b: [1, 2], c: [1, 2]}}, {_id: 1}, "save " + msg);
res = assert.writeOK(coll.update({}, {$pull: {'a.b': 2, 'a.c': 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: {b: [1], c: [1]}}, coll.findOne({}), msg);
assertLastOplog({$set: {'a.b': [1], 'a.c': [1]}}, {_id: 1}, msg);

var msg = "MultiSets: two $set";
coll.save({_id: 1, a: 1, b: 1});
assertLastOplog({_id: 1, a: 1, b: 1}, {_id: 1}, "save " + msg);
res = assert.writeOK(coll.update({}, {$set: {a: 2, b: 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: 2, b: 2}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 2, b: 2}}, {_id: 1}, msg);

// More tests to validate the oplog format and correct excution

var msg = "bad single $set";
coll.save({_id: 1, a: 1});
assertLastOplog({_id: 1, a: 1}, {_id: 1}, "save " + msg);
res = assert.writeOK(coll.update({}, {$set: {a: 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: 2}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 2}}, {_id: 1}, msg);

var msg = "bad single $inc";
res = assert.writeOK(coll.update({}, {$inc: {a: 1}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: 3}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 3}}, {_id: 1}, msg);

var msg = "bad double $set";
res = assert.writeOK(coll.update({}, {$set: {a: 2, b: 2}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: 2, b: 2}, coll.findOne({}), msg);
assertLastOplog({$set: {a: 2, b: 2}}, {_id: 1}, msg);

var msg = "bad save";
assert.writeOK(coll.save({_id: 1, a: [2]}));
assert.docEq({_id: 1, a: [2]}, coll.findOne({}), msg);
assertLastOplog({_id: 1, a: [2]}, {_id: 1}, msg);

var msg = "bad array $inc";
res = assert.writeOK(coll.update({}, {$inc: {"a.0": 1}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: [3]}, coll.findOne({}), msg);
var lastTS = assertLastOplog({$set: {"a.0": 3}}, {_id: 1}, msg);

var msg = "bad $setOnInsert";
res = assert.writeOK(coll.update({}, {$setOnInsert: {a: -1}}));
assert.eq(res.nMatched, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: [3]}, coll.findOne({}), msg);               // No-op
var otherTS = assertLastOplog({$set: {"a.0": 3}}, {_id: 1}, msg);    // Nothing new
assert.eq(lastTS, otherTS, "new oplog was not expected -- " + msg);  // No new oplog entry

coll.remove({});
assert.eq(coll.find().itcount(), 0, "collection not empty");

var msg = "bad $setOnInsert w/upsert";
res = assert.writeOK(coll.update({}, {$setOnInsert: {a: 200}}, {upsert: true}));  // upsert
assert.eq(res.nUpserted, 1, "update failed for '" + msg + "': " + res.toString());
var id = res.getUpsertedId()._id;
assert.docEq({_id: id, a: 200}, coll.findOne({}), msg);  // No-op
assertLastOplog({_id: id, a: 200}, null, msg);           // No new oplog entry

coll.remove({});
assert.eq(coll.find().itcount(), 0, "collection not empty-2");

/* inconsistent oplog format with old code -- new is okay but less efficient
 * enable once we switch the default
var msg = "bad array $push";
coll.save({_id:1, a:[1,2]})
coll.update({}, {$push:{a:3}});
var gle = cdb.getLastErrorObj();
assert.isnull(gle.err, msg);
assert.eq(gle.n, 1, "update failed for '" + msg +"': "+ tojson(gle));
assert.docEq({_id:1, a:[1,2,3]}, coll.findOne({}), msg);
//assertLastOplog({$set:{"a.2": 3}}, {_id:1}, msg); // old format
assertLastOplog({$set:{"a": [1,2,3]}}, {_id:1}, msg); // new format
 */

var msg = "bad array $push 2";
coll.save({_id: 1, a: "foo"});
res = assert.writeOK(coll.update({}, {$push: {c: 18}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: "foo", c: [18]}, coll.findOne({}), msg);
assertLastOplog({$set: {"c": [18]}}, {_id: 1}, msg);

var msg = "bad array $push $slice";
coll.save({_id: 1, a: {b: [18]}});
res = assert.writeOK(coll.update({_id: {$gt: 0}}, {$push: {"a.b": {$each: [1, 2], $slice: -2}}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: {b: [1, 2]}}, coll.findOne({}), msg);
assertLastOplog({$set: {"a.b": [1, 2]}}, {_id: 1}, msg);

var msg = "bad array $push $sort ($slice -100)";
coll.save({_id: 1, a: {b: [{c: 2}, {c: 1}]}});
res = assert.writeOK(
    coll.update({}, {$push: {"a.b": {$each: [{c: -1}], $sort: {c: 1}, $slice: -100}}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: {b: [{c: -1}, {c: 1}, {c: 2}]}}, coll.findOne({}), msg);
assertLastOplog({$set: {"a.b": [{c: -1}, {c: 1}, {c: 2}]}}, {_id: 1}, msg);

var msg = "bad array $push $slice $sort";
coll.save({_id: 1, a: [{b: 2}, {b: 1}]});
res = assert.writeOK(
    coll.update({_id: {$gt: 0}}, {$push: {a: {$each: [{b: -1}], $slice: -2, $sort: {b: 1}}}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: [{b: 1}, {b: 2}]}, coll.findOne({}), msg);
assertLastOplog({$set: {a: [{b: 1}, {b: 2}]}}, {_id: 1}, msg);

var msg = "bad array $push $slice $sort first two";
coll.save({_id: 1, a: {b: [{c: 2}, {c: 1}]}});
res = assert.writeOK(
    coll.update({_id: {$gt: 0}}, {$push: {"a.b": {$each: [{c: -1}], $slice: -2, $sort: {c: 1}}}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: {b: [{c: 1}, {c: 2}]}}, coll.findOne({}), msg);
assertLastOplog({$set: {"a.b": [{c: 1}, {c: 2}]}}, {_id: 1}, msg);

var msg = "bad array $push $slice $sort reversed first two";
coll.save({_id: 1, a: {b: [{c: 1}, {c: 2}]}});
res = assert.writeOK(
    coll.update({_id: {$gt: 0}}, {$push: {"a.b": {$each: [{c: -1}], $slice: -2, $sort: {c: -1}}}}));
assert.eq(res.nModified, 1, "update failed for '" + msg + "': " + res.toString());
assert.docEq({_id: 1, a: {b: [{c: 1}, {c: -1}]}}, coll.findOne({}), msg);
assertLastOplog({$set: {"a.b": [{c: 1}, {c: -1}]}}, {_id: 1}, msg);

replTest.stopSet();
