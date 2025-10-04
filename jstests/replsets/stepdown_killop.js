// Ensure stepDown operations that are waiting for replication can be interrupted with killOp()
// 1. Start up a 3 node set (1 arbiter).
// 2. Stop replication on the SECONDARY using a fail point.
// 3. Do one write and then spin up a second shell which asks the PRIMARY to StepDown.
// 4. Once StepDown has begun, attempt to do writes and confirm that they fail with
// NotWritablePrimary.
// 5. Kill the stepDown operation.
// 6. Writes should become allowed again and the primary should stay primary.

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

let name = "interruptStepDown";
let replSet = new ReplSetTest({name: name, nodes: 3});
let nodes = replSet.nodeList();
replSet.startSet();
replSet.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], "priority": 0},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true},
    ],
});

replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    replSet
        .getPrimary()
        .adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replSet.awaitReplication();

let secondary = replSet.getSecondary();
jsTestLog("Disable replication on the SECONDARY " + secondary.host);
stopServerReplication(secondary);

let primary = replSet.getPrimary();
assert.eq(primary.host, nodes[0], "primary assumed to be node 0");

// do a write then ask the PRIMARY to stepdown
jsTestLog("Initiating stepdown");
assert.commandWorked(
    primary
        .getDB(name)
        .foo.insert({myDoc: true, x: 1}, {writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
);
let stepDownCmd = function () {
    let res = db.getSiblingDB("admin").runCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60});
    assert.commandFailedWithCode(res, 11601 /*interrupted*/);
};
let stepDowner = startParallelShell(stepDownCmd, primary.port);
let stepDownOpID = -1;

jsTestLog("Looking for stepdown in currentOp() output");
assert.soon(function () {
    let res = primary.getDB("admin").currentOp(true);
    for (let index in res.inprog) {
        let entry = res.inprog[index];
        if (entry["command"] && entry["command"]["replSetStepDown"] === 60) {
            stepDownOpID = entry.opid;
            return true;
        }
    }
    printjson(res);
    return false;
}, "No pending stepdown command found");

jsTestLog("Ensure that writes start failing with NotWritablePrimary errors");
assert.soonNoExcept(function () {
    assert.commandFailedWithCode(primary.getDB(name).foo.insert({x: 2}), ErrorCodes.NotWritablePrimary);
    return true;
});

jsTestLog(
    "Ensure that even though writes are failing with NotWritablePrimary, we still report " + "ourselves as PRIMARY",
);
assert.eq(ReplSetTest.State.PRIMARY, primary.adminCommand("replSetGetStatus").myState);

// kill the stepDown and ensure that that unblocks writes to the db
jsTestLog("Killing stepdown");
primary.getDB("admin").killOp(stepDownOpID);

let exitCode = stepDowner();
assert.eq(0, exitCode);

assert.commandWorked(primary.getDB(name).foo.remove({}));
restartServerReplication(secondary);
replSet.stopSet();
