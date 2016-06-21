// SERVER-15310 Ensure that stepDown kills all other running operations

(function() {
    "use strict";
    var name = "stepdownKillOps";
    var replSet = new ReplSetTest({name: name, nodes: 3});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0], "priority": 3},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "arbiterOnly": true}
        ]
    });

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);

    var primary = replSet.getPrimary();
    assert.eq(primary.host, nodes[0], "primary assumed to be node 0");
    assert.writeOK(primary.getDB(name).foo.insert({x: 1}, {w: 2, wtimeout: 10000}));
    replSet.awaitReplication();

    jsTestLog("Sleeping 30 seconds so the SECONDARY will be considered electable");
    sleep(30000);

    // Run eval() in a separate thread to take the global write lock which would prevent stepdown
    // from completing if it failed to kill all running operations.
    jsTestLog("Running eval() to grab global write lock");
    var evalCmd = function() {
        db.eval(function() {
            for (var i = 0; i < 60; i++) {
                // Sleep in 1 second intervals so the javascript engine will notice when
                // it's killed
                sleep(1000);
            }
        });
    };
    var evalRunner = startParallelShell(evalCmd, primary.port);

    jsTestLog("Confirming that eval() is running and has the global lock");
    assert.soon(function() {
        var res = primary.getDB('admin').currentOp();
        for (var index in res.inprog) {
            var entry = res.inprog[index];
            if (entry["query"] && entry["query"]["$eval"]) {
                if ("W" === entry["locks"]["Global"]) {
                    return true;
                }
            }
        }
        printjson(res);
        return false;
    }, "$eval never ran and grabbed the global write lock");

    jsTestLog("Stepping down");
    try {
        assert.commandWorked(primary.getDB('admin').runCommand({replSetStepDown: 30}));
    } catch (x) {
        // expected
    }

    jsTestLog("Waiting for former PRIMARY to become SECONDARY");
    replSet.waitForState(primary, ReplSetTest.State.SECONDARY, 30000);

    var newPrimary = replSet.getPrimary();
    assert.neq(primary, newPrimary, "SECONDARY did not become PRIMARY");

    var exitCode = evalRunner({checkExitSuccess: false});
    assert.neq(
        0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");
})();
