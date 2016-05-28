// Ensure writes do not prevent a node from Stepping down
// 1. Start up a 3 node set (1 arbiter).
// 2. Stop replication on the SECONDARY using a fail point.
// 3. Do one write and then spin up a second shell which asks the PRIMARY to StepDown.
// 4. Once StepDown has begun, spin up a third shell which will attempt to do writes, which should
//    block waiting for StepDown to release its lock.
// 5. Once a write is blocked, restart replication on the SECONDARY.
// 6. Wait for PRIMARY to StepDown.

(function() {
    "use strict";
    var name = "stepDownWithLongWait";
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

    var secondary = replSet.getSecondary();
    jsTestLog('Disable replication on the SECONDARY ' + secondary.host);
    assert.commandWorked(secondary.getDB('admin').runCommand(
                             {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}),
                         'Failed to configure rsSyncApplyStop failpoint.');

    jsTestLog("do a write then ask the PRIMARY to stepdown");
    var options = {writeConcern: {w: 1, wtimeout: 60000}};
    assert.writeOK(primary.getDB(name).foo.insert({x: 1}, options));
    var stepDownSecs = 60;
    var secondaryCatchUpPeriodSecs = 60;
    var stepDownCmd = "db.getSiblingDB('admin').runCommand({" + "replSetStepDown: " + stepDownSecs +
        ", " + "secondaryCatchUpPeriodSecs: " + secondaryCatchUpPeriodSecs + "});";
    var stepDowner = startParallelShell(stepDownCmd, primary.port);

    assert.soon(function() {
        var res = primary.getDB('admin').currentOp(true);
        for (var entry in res.inprog) {
            if (res.inprog[entry]["query"] &&
                res.inprog[entry]["query"]["replSetStepDown"] === 60) {
                return true;
            }
        }
        printjson(res);
        return false;
    }, "global shared lock not acquired", 30000, 1000);

    jsTestLog("do a write and wait for it to be waiting for a lock");
    var updateCmd = function() {
        jsTestLog('Updating document on the primary. Blocks until the primary has stepped down.');
        try {
            var res = db.getSiblingDB("stepDownWithLongWait").foo.update({}, {$inc: {x: 1}});
            jsTestLog('Unexpected successful update operation on the primary during step down: ' +
                      tojson(res));
        } catch (e) {
            // Not important what error we get back. The client will probably be disconnected by
            // the primary with a "error doing query: failed" message.
            jsTestLog('Update operation returned with result: ' + tojson(e));
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
    }, "write failed to block on global lock", 30000, 1000);

    jsTestLog('Enable replication on the SECONDARY ' + secondary.host);
    assert.commandWorked(
        secondary.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}),
        'Failed to disable rsSyncApplyStop failpoint.');

    jsTestLog("Wait for PRIMARY " + primary.host + " to completely step down.");
    replSet.waitForState(primary, ReplSetTest.State.SECONDARY, secondaryCatchUpPeriodSecs * 1000);
    var exitCode = stepDowner({checkExitSuccess: false});
    assert.neq(0, exitCode, "expected replSetStepDown to close the shell's connection");

    // The connection for the 'writer' may be closed due to the primary stepping down, or signaled
    // by the main thread to quit.
    writer({checkExitSuccess: false});

    jsTestLog("Wait for SECONDARY " + secondary.host + " to become PRIMARY");
    replSet.waitForState(secondary, ReplSetTest.State.PRIMARY, stepDownSecs * 1000);
})();
