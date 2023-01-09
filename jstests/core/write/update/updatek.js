// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

// Test modifier operations on numerically equivalent string field names.  SERVER-4776

t = db.jstests_updatek;

t.drop();
t.save({_id: 0, '1': {}, '01': {}});
t.update({}, {$set: {'1.b': 1, '1.c': 2}});
assert.docEq({"01": {}, "1": {"b": 1, "c": 2}, "_id": 0}, t.findOne());

t.drop();
t.save({_id: 0, '1': {}, '01': {}});
t.update({}, {$set: {'1.b': 1, '01.c': 2}});
assert.docEq({"01": {"c": 2}, "1": {"b": 1}, "_id": 0}, t.findOne());
