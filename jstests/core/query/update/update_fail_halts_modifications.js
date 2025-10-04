// Test that update validation failure terminates the update without modifying subsequent
// documents.  SERVER-4779
// This test uses a multi-update, which is not retryable. The behavior it is testing is also not
// true of sharded clusters, since one shard may continue applying updates while the other
// encounters an error.
// @tags: [requires_multi_updates, requires_non_retryable_writes, assumes_unsharded_collection]

let t = db[jsTestName()];
t.drop();

assert.commandWorked(t.save({a: []}));
assert.commandWorked(t.save({a: 1}));
assert.commandWorked(t.save({a: []}));

assert.writeError(t.update({}, {$push: {a: 2}}, false, true));
assert.eq(1, t.count({a: 2}));
