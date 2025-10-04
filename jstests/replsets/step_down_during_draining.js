// Test stepdown during drain mode
// 1. Set up a 3-node set. Assume Node 1 is the primary at the beginning for simplicity.
// 2. Prevent applying retrieved ops on all secondaries, including Node 2.
// 3. Insert data to ensure Node 2 has ops to apply in its queue.
// 4. Step up Node 2. Now it enters drain mode, but cannot proceed.
// 5. Step up Node 1. Wait until Node 2 knows of a higher term and steps down.
//    Node 2 re-enables bgsync producer while it's still in drain mode.
// 6. Step up Node 2 again. It enters drain mode again.
// 7. Enable applying ops.
// 8. Ensure the ops in queue are applied and that Node 2 begins to accept writes as usual.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

let replSet = new ReplSetTest({name: "testSet", nodes: 3});
let nodes = replSet.nodeList();
replSet.startSet();
let conf = replSet.getReplSetConfig();
conf.members[2].priority = 0;
conf.settings = conf.settings || {};
conf.settings.chainingAllowed = false;
conf.settings.catchUpTimeoutMillis = 0;
replSet.initiate(conf);

let primary = replSet.getPrimary();
let secondary = replSet.getSecondary();

// Set verbosity for replication on all nodes.
let verbosity = {
    "setParameter": 1,
    "logComponentVerbosity": {
        "replication": {"verbosity": 3},
    },
};
replSet.nodes.forEach(function (node) {
    node.adminCommand(verbosity);
});

function enableFailPoint(node) {
    jsTest.log("enable failpoint " + node.host);
    assert.commandWorked(node.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
    // Wait for Oplog Applier to hang on the failpoint.
    checkLog.contains(node, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");
}

function disableFailPoint(node) {
    jsTest.log("disable failpoint " + node.host);
    assert.commandWorked(node.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
}

// The default WC is majority and rsSyncApplyStop failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Do an initial insert to prevent the secondary from going into recovery
let numDocuments = 20;
let coll = primary.getDB("foo").foo;
assert.commandWorked(coll.insert({x: 0}, {writeConcern: {w: 3}}));
replSet.awaitReplication();

// Enable fail point to stop replication.
let secondaries = replSet.getSecondaries();
secondaries.forEach(enableFailPoint);

const reduceMajorityWriteLatency = FeatureFlagUtil.isPresentAndEnabled(secondary, "ReduceMajorityWriteLatency");
let bufferCountBefore = reduceMajorityWriteLatency
    ? secondary.getDB("foo").serverStatus().metrics.repl.buffer.write.count
    : secondary.getDB("foo").serverStatus().metrics.repl.buffer.count;
for (let i = 1; i < numDocuments; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
jsTestLog("Number of documents inserted into collection on primary: " + numDocuments);
assert.eq(numDocuments, primary.getDB("foo").foo.find().itcount());

assert.soon(
    function () {
        let serverStatus = secondary.getDB("foo").serverStatus();
        let bufferCount = reduceMajorityWriteLatency
            ? serverStatus.metrics.repl.buffer.write.count
            : serverStatus.metrics.repl.buffer.count;
        let bufferCountChange = bufferCount - bufferCountBefore;
        jsTestLog("Number of operations buffered on secondary since stopping applier: " + bufferCountChange);
        return bufferCountChange == numDocuments - 1;
    },
    "secondary did not buffer operations for new inserts on primary",
    ReplSetTest.kDefaultTimeoutMS,
    1000,
);

reconnect(secondary);
replSet.stepUp(secondary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});

// Secondary doesn't allow writes yet.
let res = secondary.getDB("admin").runCommand({"hello": 1});
assert(!res.isWritablePrimary);

// Original primary steps up.
reconnect(primary);
replSet.stepUp(primary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});

reconnect(secondary);
replSet.stepUp(secondary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});

// Disable fail point to allow replication.
secondaries.forEach(disableFailPoint);

// Wait for draining to complete.
const newPrimary = replSet.getPrimary();
assert.eq(secondary, newPrimary);

// Ensure new primary is writable, and wait until the write has been replicated to all nodes, to
// allow rollback (should it occur), to complete.
jsTestLog("New primary should be writable after draining is complete");
assert.commandWorked(newPrimary.getDB("foo").flag.insert({sentinel: 1}, {writeConcern: {w: 3}}));
// Check that all writes reached the secondary's op queue prior to
// stepping down the original primary and got applied.
assert.eq(newPrimary.getDB("foo").foo.find().itcount(), numDocuments);
replSet.stopSet();
