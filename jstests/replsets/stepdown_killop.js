// Ensure stepDown operations that are waiting for replication can be interrupted with killOp()
// 1. Start up a 3 node set (1 arbiter).
// 2. Stop replication on the SECONDARY using a fail point.
// 3. Do one write and then spin up a second shell which asks the PRIMARY to StepDown.
// 4. Once StepDown has begun, spin up a third shell which will attempt to do writes, which should
//    block waiting for stepDown to release its lock, which it never will do because no secondaries
//    are caught up.
// 5. Once a write is blocked, kill the stepDown operation
// 6. Writes should become unblocked and the primary should stay primary

(function() {
    "use strict";
    var name = "interruptStepDown";
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

    var secondary = replSet.getSecondary();
    jsTestLog('Disable replication on the SECONDARY ' + secondary.host);
    assert.commandWorked(secondary.getDB('admin').runCommand(
                             {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}),
                         'Failed to configure rsSyncApplyStop failpoint.');

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);

    var primary = replSet.getPrimary();
    assert.eq(primary.host, nodes[0], "primary assumed to be node 0");

    // do a write then ask the PRIMARY to stepdown
    jsTestLog("Initiating stepdown");
    assert.writeOK(primary.getDB(name).foo.insert({myDoc: true, x: 1},
                                                  {writeConcern: {w: 1, wtimeout: 60000}}));
    var stepDownCmd = function() {
        var res = db.getSiblingDB('admin').runCommand(
            {replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60});
        assert.commandFailedWithCode(res, 11601 /*interrupted*/);
    };
    var stepDowner = startParallelShell(stepDownCmd, primary.port);
    var stepDownOpID = -1;

    jsTestLog("Looking for stepdown in currentOp() output");
    assert.soon(function() {
        var res = primary.getDB('admin').currentOp(true);
        for (var index in res.inprog) {
            var entry = res.inprog[index];
            if (entry["query"] && entry["query"]["replSetStepDown"] === 60) {
                stepDownOpID = entry.opid;
                return true;
            }
        }
        printjson(res);
        return false;
    }, "global shared lock not acquired");

    jsTestLog("Ensuring writes block on the stepdown");
    // Start repeatedly doing an update until one blocks waiting for the lock.
    // If the test is successful this thread will be terminated when we remove the document
    // being updated.
    var updateCmd = function() {
        while (true) {
            var res =
                db.getSiblingDB("interruptStepDown").foo.update({myDoc: true}, {$inc: {x: 1}});
            assert.writeOK(res);
            if (res.nModified == 0) {
                quit(0);
            } else {
                printjson(res);
            }
        }
    };
    var writer = startParallelShell(updateCmd, primary.port);
    assert.soon(function() {
        var res = primary.getDB(name).currentOp();
        for (var entry in res.inprog) {
            if (res.inprog[entry]["waitingForLock"]) {
                return true;
            }
        }
        printjson(res);
        return false;
    }, "write never blocked on the global shared lock");

    // kill the stepDown and ensure that that unblocks writes to the db
    jsTestLog("Killing stepdown");
    primary.getDB('admin').killOp(stepDownOpID);

    var exitCode = stepDowner();
    assert.eq(0, exitCode);

    assert.writeOK(primary.getDB(name).foo.remove({}));
    exitCode = writer();
    assert.eq(0, exitCode);
})();
