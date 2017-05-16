// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// from db.eval.
// @tags: [assumes_unsharded_collection]

t = db.eval_nolock;
t.drop();

for (i = 0; i < 10; i++)
    t.insert({_id: i});

res = db.runCommand({
    eval: function() {
        db.eval_nolock.insert({_id: 123});
        return db.eval_nolock.count();
    },
    nolock: true
});

assert.eq(11, res.retval, "A");
