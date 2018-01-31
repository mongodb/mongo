// SERVER-17499: Test behavior of getMore on aggregation cursor under eval command.
//
// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]
db.evalg.drop();
for (var i = 0; i < 102; ++i) {
    db.evalg.insert({});
}
assert.eq(102, db.eval(function() {
    var cursor = db.evalg.aggregate();
    assert(cursor.hasNext());
    assert.eq(101, cursor.objsLeftInBatch());
    return cursor.itcount();
}));
