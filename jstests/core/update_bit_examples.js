// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection, requires_non_retryable_writes]

// Basic examples for $bit
var res;
var coll = db.update_bit;
coll.drop();

// $bit and
coll.remove({});
coll.save({_id: 1, a: NumberInt(2)});
res = coll.update({}, {$bit: {a: {and: NumberInt(4)}}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 0);

// $bit or
coll.remove({});
coll.save({_id: 1, a: NumberInt(2)});
res = coll.update({}, {$bit: {a: {or: NumberInt(4)}}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 6);

// $bit xor
coll.remove({});
coll.save({_id: 1, a: NumberInt(0)});
res = coll.update({}, {$bit: {a: {xor: NumberInt(4)}}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 4);

// SERVER-19706 Empty bit operation.
res = coll.update({}, {$bit: {a: {}}});
assert.writeError(res);
