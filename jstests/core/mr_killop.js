// Test killop applied to m/r operations and child ops of m/r operations.

t = db.jstests_mr_killop;
t.drop();
t2 = db.jstests_mr_killop_out;
t2.drop();
db.adminCommand({"configureFailPoint": 'mr_killop_test_fp', "mode": 'alwaysOn'});
function debug(x) {
    //        printjson( x );
}

/** @return op code for map reduce op created by spawned shell, or that op's child */
function op(childLoop) {
    p = db.currentOp().inprog;
    debug(p);
    for (var i in p) {
        var o = p[i];
        // Identify a map/reduce or where distinct operation by its collection, whether or not
        // it is currently active.
        if (childLoop) {
            if ((o.active || o.waitingForLock) && o.query && o.query.query &&
                o.query.query.$where && o.query.distinct == "jstests_mr_killop") {
                return o.opid;
            }
        } else {
            if ((o.active || o.waitingForLock) && o.query && o.query.mapreduce &&
                o.query.mapreduce == "jstests_mr_killop") {
                return o.opid;
            }
        }
    }
    return -1;
}

/**
* Run one map reduce with the specified parameters in a parallel shell, kill the
* map reduce op or its child op with killOp, and wait for the map reduce op to
* terminate.
* @param childLoop - if true, a distinct $where op is killed rather than the map reduce op.
* This is necessay for a child distinct $where of a map reduce op because child
* ops currently mask parent ops in currentOp.
*/
function testOne(map, reduce, finalize, scope, childLoop, wait) {
    debug("testOne - map = " + tojson(map) + "; reduce = " + tojson(reduce) + "; finalize = " +
          tojson(finalize) + "; scope = " + tojson(scope) + "; childLoop = " + childLoop +
          "; wait = " + wait);

    t.drop();
    t2.drop();
    // Ensure we have 2 documents for the reduce to run
    t.save({a: 1});
    t.save({a: 1});

    spec = {mapreduce: "jstests_mr_killop", out: "jstests_mr_killop_out", map: map, reduce: reduce};
    if (finalize) {
        spec["finalize"] = finalize;
    }
    if (scope) {
        spec["scope"] = scope;
    }

    // Windows shell strips all double quotes from command line, so use
    // single quotes.
    stringifiedSpec = tojson(spec).toString().replace(/\n/g, ' ').replace(/\"/g, "\'");

    // The assert below won't be caught by this test script, but it will cause error messages
    // to be printed.
    var awaitShell =
        startParallelShell("assert.commandWorked( db.runCommand( " + stringifiedSpec + " ) );");

    if (wait) {
        sleep(2000);
    }

    o = null;
    assert.soon(function() {
        o = op(childLoop);
        return o != -1;
    });

    res = db.killOp(o);
    debug("did kill : " + tojson(res));

    // When the map reduce op is killed, the spawned shell will exit
    var exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(0,
               exitCode,
               "expected shell to exit abnormally due to map-reduce execution being terminated");
    debug("parallel shell completed");

    assert.eq(-1, op(childLoop));
}

/** Test using wait and non wait modes */
function test(map, reduce, finalize, scope, childLoop) {
    debug(" Non wait mode");
    testOne(map, reduce, finalize, scope, childLoop, false);

    debug(" Wait mode");
    testOne(map, reduce, finalize, scope, childLoop, true);
}

/** Test looping in map and reduce functions */
function runMRTests(loop, childLoop) {
    debug(" Running MR test - loop map function. no scope ");
    test(loop,  // map
         function(k, v) {
             return v[0];
         },     // reduce
         null,  // finalize
         null,  // scope
         childLoop);

    debug(" Running MR test - loop reduce function ");
    test(
        function() {
            emit(this.a, 1);
        },     // map
        loop,  // reduce
        null,  // finalize
        null,  // scope
        childLoop);

    debug(" Running finalization test - loop map function. with scope ");
    test(
        function() {
            loop();
        },  // map
        function(k, v) {
            return v[0];
        },             // reduce
        null,          // finalize
        {loop: loop},  // scope
        childLoop);
}

/** Test looping in finalize function */
function runFinalizeTests(loop, childLoop) {
    debug(" Running finalization test - no scope ");
    test(
        function() {
            emit(this.a, 1);
        },  // map
        function(k, v) {
            return v[0];
        },     // reduce
        loop,  // finalize
        null,  // scope
        childLoop);

    debug(" Running finalization test - with scope ");
    test(
        function() {
            emit(this.a, 1);
        },  // map
        function(k, v) {
            return v[0];
        },  // reduce
        function(a, b) {
            loop();
        },             // finalize
        {loop: loop},  // scope
        childLoop);
}

// Run inside server. No access to debug().
var loop = function() {
    while (1) {
        sleep(1000);
    }
};
runMRTests(loop, false);
runFinalizeTests(loop, false);
db.adminCommand({"configureFailPoint": 'mr_killop_test_fp', "mode": 'off'});
