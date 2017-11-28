// @tags: [does_not_support_stepdowns]

// Tests the use of a match predicate with the currentOp command.
(function() {
    // Test a predicate that matches the currentOp operation we are running.
    var res = db.adminCommand("currentOp", {command: {$exists: true}});
    assert.commandWorked(res);
    assert.gt(res.inprog.length, 0, tojson(res));

    // Test a predicate that matches no operations.
    res = db.adminCommand("currentOp", {dummyCurOpField: {exists: true}});
    assert.commandWorked(res);
    assert.eq(res.inprog.length, 0, tojson(res));
})();
