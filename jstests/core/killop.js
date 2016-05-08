/**
 * Basic test of killop functionality.
 *
 * Theory of operation: Creates two operations that will take a long time, sends killop for those
 * operations, and then attempts to infer that the operations died because of killop, and not for
 * some other reason.
 *
 * NOTES:
 * The long operations are count({ $where: function() { while (1) { sleep(500); } } }).  These
 * operations do not terminate until the server determines that they've spent too much time in JS
 * execution, typically after 30 seconds of wall clock time have passed.  For these operations to
 * take a long time, the counted collection must not be empty; hence an initial write to the
 * collection is required.
 */
(function() {
    'use strict';

    var t = db.jstests_killop;
    t.save({x: 1});

    /**
     * This function filters for the operations that we're looking for, based on their state and
     * the contents of their query object.
     */
    function ops() {
        var p = db.currentOp().inprog;
        var ids = [];
        for (var i in p) {
            var o = p[i];
            // We *can't* check for ns, b/c it's not guaranteed to be there unless the query is
            // active, which it may not be in our polling cycle - particularly b/c we sleep every
            // second in both the query and the assert
            if ((o.active || o.waitingForLock) && o.query && o.query.query &&
                o.query.query.$where && o.query.count == "jstests_killop") {
                ids.push(o.opid);
            }
        }
        return ids;
    }

    var countWithWhereOp =
        'db.jstests_killop.count({ $where: function() { while (1) { sleep(500); } } });';

    jsTestLog("Starting long-running $where operation");
    var s1 = startParallelShell(countWithWhereOp);
    var s2 = startParallelShell(countWithWhereOp);

    jsTestLog("Finding ops in currentOp() output");
    var o = [];
    assert.soon(
        function() {
            o = ops();
            return o.length == 2;
        },
        {
          toString: function() {
              return tojson(db.currentOp().inprog);
          }
        },
        60000);

    var start = new Date();
    jsTestLog("Killing ops");
    db.killOp(o[0]);
    db.killOp(o[1]);

    jsTestLog("Waiting for ops to terminate");
    [s1, s2].forEach(function(awaitShell) {
        var exitCode = awaitShell({checkExitSuccess: false});
        assert.neq(
            0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");
    });

    // don't want to pass if timeout killed the js function.
    var end = new Date();
    var diff = end - start;
    assert.lt(diff, 30000, "Start: " + start + "; end: " + end + "; diff: " + diff);
})();
