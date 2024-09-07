// Test killop applied to m/r operations and child ops of m/r operations.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapreduce.
//   not_allowed_with_signed_security_token,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
//   uses_multiple_connections,
//   uses_parallel_shell,
//   no_selinux,
//   requires_scripting,
// ]
const source = db.jstests_mr_killop;
source.drop();
const out = db.jstests_mr_killop_out;
out.drop();
assert.commandWorked(db.adminCommand({configureFailPoint: "mr_killop_test_fp", mode: "alwaysOn"}));

/** @return op code for map reduce op created by spawned shell. */
function getOpCode() {
    const inProg = db.currentOp().inprog;

    function isMapReduce(op) {
        if (!op.command) {
            return false;
        }

        if (TestData.testingReplicaSetEndpoint && op.role == "ClusterRole{shard}") {
            // On the replica set endpoint, currentOp reports both router and shard operations. To
            // interrupt the mapReduce operation, the killOp command must be use the opId of the
            // router mapReduce operation.
            return false;
        }

        const cmdBody = op.command;
        if (cmdBody.$truncated) {
            const stringifiedCmd = cmdBody.$truncated;
            return (stringifiedCmd.search('mapreduce') >= 0 ||
                    stringifiedCmd.search('aggregate') >= 0) &&
                stringifiedCmd.search(source.getName()) >= 0;
        }

        return (cmdBody.mapreduce && cmdBody.mapreduce == source.getName()) ||
            (cmdBody.isMapReduceCommand && cmdBody.aggregate == source.getName());
    }

    for (let i in inProg) {
        const o = inProg[i];
        // Identify a map/reduce operation by its collection, whether or not it is currently active.
        if ((o.active || o.waitingForLock) && isMapReduce(o))
            return o.opid;
    }
    return -1;
}

/**
 * Run one mapReduce with the specified parameters in a parallel shell. Kill the map reduce op and
 * wait for the map reduce op to terminate.
 */
function runTest(map, reduce, finalize, scope, wait) {
    source.drop();
    out.drop();
    // Ensure we have 2 documents for the reduce to run.
    assert.commandWorked(source.insert({a: 1}));
    assert.commandWorked(source.insert({a: 1}));

    const spec = {mapreduce: source.getName(), out: out.getName(), map: map, reduce: reduce};
    if (finalize) {
        spec["finalize"] = finalize;
    }
    if (scope) {
        spec["scope"] = scope;
    }

    // Windows shell strips all double quotes from command line, so use single quotes.
    const stringifiedSpec = tojson(spec).toString().replace(/\n/g, ' ').replace(/\"/g, "\'");

    // The assert below won't be caught by this test script, but it will cause error messages to be
    // printed.
    const awaitShell =
        startParallelShell("assert.commandWorked( db.runCommand( " + stringifiedSpec + " ) );");

    if (wait) {
        sleep(20);
    }

    let opCode = null;
    assert.soon(function() {
        opCode = getOpCode();
        return opCode != -1;
    });

    db.killOp(opCode);

    // When the map reduce op is killed, the spawned shell will exit
    const exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(0,
               exitCode,
               "expected shell to exit abnormally due to map-reduce execution being terminated");
    assert.eq(-1, getOpCode());
}

/** Test using wait and non wait modes. */
function runTests(map, reduce, finalize, scope) {
    runTest(map, reduce, finalize, scope, false);
    runTest(map, reduce, finalize, scope, true);
}

/** Test looping in map function. */
function runMapTests(loop) {
    // Without scope.
    runTests(
        loop,  // map
        function(k, v) {
            return v[0];
        },     // reduce
        null,  // finalize
        null   // scope
    );

    // With scope.
    runTests(
        function() {
            loop();
        },  // map
        function(k, v) {
            return v[0];
        },            // reduce
        null,         // finalize
        {loop: loop}  // scope
    );
}

/** Test looping in reduce function. */
function runReduceTests(loop) {
    // Without scope.
    runTests(
        function() {
            emit(this.a, 1);
        },     // map
        loop,  // reduce
        null,  // finalize
        null   // scope
    );

    // With scope.
    runTests(
        function() {
            emit(this.a, 1);
        },  // map
        function() {
            loop();
        },            // reduce
        null,         // finalize
        {loop: loop}  // scope
    );
}

/** Test looping in finalize function. */
function runFinalizeTests(loop) {
    // Without scope.
    runTests(
        function() {
            emit(this.a, 1);
        },  // map
        function(k, v) {
            return v[0];
        },     // reduce
        loop,  // finalize
        null   // scope
    );

    // With scope.
    runTests(
        function() {
            emit(this.a, 1);
        },  // map
        function(k, v) {
            return v[0];
        },  // reduce
        function(a, b) {
            loop();
        },            // finalize
        {loop: loop}  // scope
    );
}

const loop = function() {
    while (1) {
        sleep(10);
    }
};
runMapTests(loop, false);
runReduceTests(loop, false);
runFinalizeTests(loop, false);
db.adminCommand({configureFailPoint: "mr_killop_test_fp", mode: "off"});
