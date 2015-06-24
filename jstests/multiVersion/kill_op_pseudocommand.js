(function() {
    "use strict";
    // Test that a 3.2 Mongos will correctly translate a killOp command
    // to a killop pseudocommand when talking to an old shard

    // TODO: Remove after mongodb 3.2 is released

    // Sharded cluster
    // -- latest mongos
    // -- latest config
    // -- one 3.0 shard
    var options = {
      mongosOptions: {binVersion: "3.1"},
      configOptions: {binVersion: "3.1"},
      shardOptions: {binVersion: "3.0"}
    };

    var st = new ShardingTest({name: "killOp-multiver", shards: 1, other: options});

    var db = st.s.getDB("killOp-multiver");
    var db1 = db;
    db.dropDatabase();
    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

    var testCol = db.tc;

    // start long running op
    testCol.insert({"foo": "bar"});
    jsTestLog("Starting long-running $where operation");

    var start = new Date();

    var parShell = startParallelShell(
      'db.getSiblingDB("killOp-multiver").tc.count( { $where: function() { while( 1 ) { ; } }})',
      st.s.port);
    var findOpId = function () {
        var curOps = db.currentOp();
        var inProg = curOps.inprog;
        var opId = null;
        inProg.forEach(function(op) {
            if ((op.active === true) &&
                (op.ns === "killOp-multiver.tc") &&
                (op.query.count === "tc"))  {

                opId = op.opid;
            }
        });
        return opId;
    };

    var opToKill = null;

    do {
      opToKill = findOpId()
      sleep(25);
    } while (opToKill === null);

    db.killOp(opToKill);

    var exitCode = parShell({checkExitSuccess: false}); // wait for query to end
    assert.neq(0, exitCode,
               "expected shell to exit abnormally due to JS execution being terminated");

    var end = new Date();

    // make sure the query didn't end due to js op timeout
    assert.lt(diff, 30000, "Query was killed due to timeout - not killOp");
    st.stop();
})();
