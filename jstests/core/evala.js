// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// from db.eval.
// @tags: [assumes_unsharded_collection]

t = db.evala;
t.drop();

t.save({x: 5});

assert.eq(5, db.eval("function(){ return db.evala.findOne().x; }"), "A");
assert.eq(5, db.eval("/* abc */function(){ return db.evala.findOne().x; }"), "B");
