// Tests the use of a match predicate with the currentOp command.
(function() {
    // Test a $where predicate that matches the currentOp operation we are running.
    var res = db.adminCommand("currentOp", {
        $where: function() {
            return true;
        }
    });
    assert.commandWorked(res);
    assert.gt(res.inprog.length, 0, tojson(res));

    // Test a $where predicate that matches no operations.
    res = db.adminCommand("currentOp", {
        $where: function() {
            return false;
        }
    });
    assert.commandWorked(res);
    assert.eq(res.inprog.length, 0, tojson(res));
})();
