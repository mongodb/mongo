// Ensure writes do not prevent a node from Stepping down
// 1. Start up a 3 node set (1 arbiter).
// 2. Isolate the SECONDARY.
// 3. Do one write and then spin up a second shell which asks the PRIMARY to StepDown.
// 4. Once StepDown has begun, spin up a third shell which will attempt to do writes, which should
//    block waiting for StepDown to release its lock.
// 5. Once a write is blocked, de-isolate the SECONDARY.
// 6. Wait for PRIMARY to StepDown.

(function () {
    "use strict";
    var name = "stepDownWithLongWait";
    var replSet = new ReplSetTest({name: name, nodes: 3});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate({"_id" : name,
                      "members" : [
                          {"_id" : 0, "host" : nodes[0], "priority" : 3},
                          {"_id" : 1, "host" : nodes[1]},
                          {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

    jsTestLog("isolate one node");
    replSet.bridge();
    replSet.partition(1,0);
    replSet.partition(1,2);

    replSet.waitForState(replSet.nodes[0], replSet.PRIMARY, 60 * 1000);
    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();
    assert.eq(primary.host, nodes[0], "primary assumed to be node 0");

    jsTestLog("do a write then ask the PRIMARY to stepdown");
    var options = {writeConcern: {w: 1, wtimeout: 60000}};
    assert.writeOK(primary.getDB(name).foo.insert({x: 1}, options));
    var cmd = "db.getSiblingDB('admin').runCommand({replSetStepDown: 60, " +
                                                   "secondaryCatchUpPeriodSecs: 60});";
    var stepDowner = startParallelShell(cmd, primary.port);
    assert.soon(function() {
        var res = primary.getDB('admin').currentOp(true);
        for (var entry in res.inprog) {
            if (res.inprog[entry]["query"] && res.inprog[entry]["query"]["replSetStepDown"] === 60){
                return true;
            }
        }
        printjson(res);
        return false;
    }, "global shared lock not acquired");

    jsTestLog("do a write and wait for it to be waiting for a lock");
    var updateCmd = function() {
        while (true) {
            var res = db.getSiblingDB("stepDownWithLongWait").foo.update({}, {$inc: {x: 1}});
            assert.writeOK(res);
            if (res.nModified === 0) {
                // main thread signaled update thread to exit by deleting the doc it's updating
                quit(0);
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
    }, "write failed to block on global lock");

    jsTestLog("bring back the SECONDARY and wait for it become PRIMARY");
    replSet.unPartition(1,0);
    replSet.unPartition(1,2);

    replSet.waitForState(secondary, replSet.PRIMARY, 30000);

    jsTestLog("signal update thread to exit");
    var newPrimary = replSet.getPrimary();
    assert.writeOK(newPrimary.getDB(name).foo.remove({}));

    var exitCode = stepDowner({checkExitSuccess: false});
    assert.neq(0, exitCode, "expected replSetStepDown to close the shell's connection");

    // The connection for the 'writer' may be closed due to the primary stepping down, or signaled
    // by the main thread to quit.
    writer({checkExitSuccess: false});
})();
