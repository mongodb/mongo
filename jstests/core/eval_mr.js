// Test that the eval command can't be used to invoke the mapReduce command.  SERVER-17889.
(function() {
    "use strict";
    db.eval_mr.drop();
    db.eval_mr_out.drop();
    assert.writeOK(db.eval_mr.insert({val: 1}));
    assert.writeOK(db.eval_mr.insert({val: 2}));
    var runBasicMapReduce = function() {
        return db.eval_mr.runCommand("mapReduce", {
            map: function() {
                emit(0, this.val);
            },
            reduce: function(id, values) {
                return Array.sum(values);
            },
            out: {replace: "eval_mr_out"}
        });
    };
    assert.commandWorked(runBasicMapReduce());
    assert.eq(3, db.eval_mr_out.findOne().value);
    assert.commandFailed(db.eval(runBasicMapReduce));
})();
