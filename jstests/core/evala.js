// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]

t = db.evala;
t.drop();

t.save({x: 5});

assert.eq(5, db.eval("function(){ return db.evala.findOne().x; }"), "A");
assert.eq(5, db.eval("/* abc */function(){ return db.evala.findOne().x; }"), "B");
