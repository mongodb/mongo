// Tests the output of db.getReplicationInfo(), db.printSlaveReplicationInfo(), and the latter's
// alias, db.printSecondaryReplicationInfo().

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "getReplicationInfo";
const replSet = new ReplSetTest({name: name, nodes: 2, oplogSize: 50});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();

// Test that db.printSlaveReplicationInfo() and db.printSecondaryReplicationInfo() both print
// out initial sync info when called during an initial sync.
const syncTarget = replSet.add({
    rsConfig: {votes: 0, priority: 0},
    setParameter: {
        "failpoint.forceSyncSourceCandidate": tojson({mode: "alwaysOn", data: {hostAndPort: primary.name}}),
    },
});
syncTarget.setSecondaryOk();
const failPointBeforeFinish = configureFailPoint(syncTarget, "initialSyncHangBeforeFinish");
replSet.reInitiate();

failPointBeforeFinish.wait();
const callPrintSecondaryReplInfo = startParallelShell(
    "db.getSiblingDB('admin').printSecondaryReplicationInfo();",
    syncTarget.port,
);
callPrintSecondaryReplInfo();
assert(rawMongoProgramOutput("InitialSyncSyncSource: ").match(primary.name));
let subStr = "InitialSyncRemainingEstimatedDuration: ";
assert(rawMongoProgramOutput(subStr).match(subStr));
clearRawMongoProgramOutput();

const callPrintSlaveReplInfo = startParallelShell(
    "db.getSiblingDB('admin').printSlaveReplicationInfo();",
    syncTarget.port,
);
callPrintSlaveReplInfo();
assert(rawMongoProgramOutput("InitialSyncSyncSource: ").match(primary.name));
assert(rawMongoProgramOutput(subStr).match(subStr));
clearRawMongoProgramOutput();
failPointBeforeFinish.off();
replSet.awaitSecondaryNodes();

for (var i = 0; i < 100; i++) {
    primary.getDB("test").foo.insert({a: i});
}
replSet.awaitReplication();

let replInfo = primary.getDB("admin").getReplicationInfo();
let replInfoString = tojson(replInfo);

assert.eq(50, replInfo.logSizeMB, replInfoString);
assert.lt(0, replInfo.usedMB, replInfoString);
assert.lte(0, replInfo.timeDiff, replInfoString);
assert.lte(0, replInfo.timeDiffHours, replInfoString);
// Just make sure the following fields exist since it would be hard to predict their values
assert(replInfo.tFirst, replInfoString);
assert(replInfo.tLast, replInfoString);
assert(replInfo.now, replInfoString);

// calling this function with and without a primary, should provide sufficient code coverage
// to catch any JS errors
let mongo = startParallelShell("db.getSiblingDB('admin').printSlaveReplicationInfo();", primary.port);
mongo();
subStr = "behind the primary";
assert(rawMongoProgramOutput(subStr).match(subStr));

// get to a primaryless state
for (i in replSet.getSecondaries()) {
    let secondary = replSet.getSecondaries()[i];
    secondary.getDB("admin").runCommand({replSetFreeze: 120});
}
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 120, force: true}));

// printSlaveReplicationInfo is deprecated and aliased to printSecondaryReplicationInfo, but ensure
// it still works for backwards compatibility.
mongo = startParallelShell("db.getSiblingDB('admin').printSlaveReplicationInfo();", primary.port);
mongo();
subStr = "behind the freshest";
assert(rawMongoProgramOutput(subStr).match(subStr));

clearRawMongoProgramOutput();
assert.eq(rawMongoProgramOutput(subStr).match(subStr), null);

// Ensure that the new helper, printSecondaryReplicationInfo works the same.
mongo = startParallelShell("db.getSiblingDB('admin').printSecondaryReplicationInfo();", primary.port);
mongo();
assert(rawMongoProgramOutput(subStr).match(subStr));

replSet.stopSet();
