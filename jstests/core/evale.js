// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]

t = db.jstests_evale;
t.drop();

db.eval(function() {
    return db.jstests_evale.count({
        $where: function() {
            return true;
        }
    });
});
db.eval("db.jstests_evale.count( { $where:function() { return true; } } )");
