// Ensure stepDown operations that are waiting for replication can be interrupted with killOp()
// 1. Start up a 3 node set (1 arbiter).
// 2. Stop replication on the SECONDARY using a fail point.
// 3. Do one write and then spin up a second shell which asks the PRIMARY to StepDown.
// 4. Once StepDown has begun, attempt to do writes and confirm that they fail with NotMaster.
// 5. Kill the stepDown operation.
// 6. Writes should become allowed again and the primary should stay primary.

(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");

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

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);

    var secondary = replSet.getSecondary();
    jsTestLog('Disable replication on the SECONDARY ' + secondary.host);
    stopServerReplication(secondary);

    var primary = replSet.getPrimary();
    assert.eq(primary.host, nodes[0], "primary assumed to be node 0");

    // do a write then ask the PRIMARY to stepdown
    jsTestLog("Initiating stepdown");
    assert.writeOK(primary.getDB(name).foo.insert(
        {myDoc: true, x: 1}, {writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
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
            if (entry["command"] && entry["command"]["replSetStepDown"] === 60) {
                stepDownOpID = entry.opid;
                return true;
            }
        }
        printjson(res);
        return false;
    }, "No pending stepdown command found");

    jsTestLog("Ensure that writes start failing with NotMaster errors");
    assert.soonNoExcept(function() {
        assert.writeErrorWithCode(primary.getDB(name).foo.insert({x: 2}), ErrorCodes.NotMaster);
        return true;
    });

    jsTestLog("Ensure that even though writes are failing with NotMaster, we still report " +
              "ourselves as PRIMARY");
    assert.eq(ReplSetTest.State.PRIMARY, primary.adminCommand('replSetGetStatus').myState);

    // kill the stepDown and ensure that that unblocks writes to the db
    jsTestLog("Killing stepdown");
    primary.getDB('admin').killOp(stepDownOpID);

    var exitCode = stepDowner();
    assert.eq(0, exitCode);

    assert.writeOK(primary.getDB(name).foo.remove({}));
    restartServerReplication(secondary);
    replSet.stopSet();
})();
