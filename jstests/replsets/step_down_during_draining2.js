// Test stepdown during drain mode
// 1. Set up a 3-node set. Assume Node 0 is the primary at the beginning for simplicity.
// 2. Prevent applying retrieved ops on all secondaries, including Node 1.
// 3. Insert data to ensure Node 1 has ops to apply in its queue.
// 4. Step up Node 1. Now it enters drain mode, but cannot proceed.
// 5. Block Node 1's ability to process stepdowns.
// 5. Shut down nodes 0 and 2. Wait until Node 1 begins stepping down due to no longer having a
//    majority
// 6. Re-enable Node 1's ability to apply operations, ensure that clearing it's buffer doesn't
//    cause it to finish drain mode because of the pending stepdown request.
// 7. Allow Node 1 to finish stepping down.

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {reconnect} from "jstests/replsets/rslib.js";

var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
var nodes = replSet.nodeList();
replSet.startSet();
var conf = replSet.getReplSetConfig();
conf.members[2].priority = 0;
conf.settings = conf.settings || {};
conf.settings.chainingAllowed = false;
conf.settings.catchUpTimeoutMillis = 0;
replSet.initiate(conf);

var primary = replSet.getPrimary();
var secondary = replSet.getSecondary();

// Set verbosity for replication on all nodes.
var verbosity = {
    "setParameter": 1,
    "logComponentVerbosity": {
        "replication": {"verbosity": 3},
    }
};
replSet.nodes.forEach(function(node) {
    node.adminCommand(verbosity);
});

function enableFailPoint(node) {
    jsTest.log("enable failpoint " + node.host);
    assert.commandWorked(
        node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));
}

function disableFailPoint(node) {
    jsTest.log("disable failpoint " + node.host);
    assert.commandWorked(node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));
}

// The default WC is majority and rsSyncApplyStop failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Do an initial insert to prevent the secondary from going into recovery
var numDocuments = 20;
var coll = primary.getDB("foo").foo;
assert.commandWorked(coll.insert({x: 0}, {writeConcern: {w: 3}}));
replSet.awaitReplication();

// Enable fail point to stop replication.
var secondaries = replSet.getSecondaries();
secondaries.forEach(enableFailPoint);

const reduceMajorityWriteLatency =
    FeatureFlagUtil.isPresentAndEnabled(secondary, "ReduceMajorityWriteLatency");
var bufferCountBefore = (reduceMajorityWriteLatency)
    ? secondary.getDB('foo').serverStatus().metrics.repl.buffer.apply.count
    : secondary.getDB('foo').serverStatus().metrics.repl.buffer.count;
for (var i = 1; i < numDocuments; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
jsTestLog('Number of documents inserted into collection on primary: ' + numDocuments);
assert.eq(numDocuments, primary.getDB("foo").foo.find().itcount());

assert.soon(
    function() {
        var serverStatus = secondary.getDB('foo').serverStatus();
        var bufferCount = (reduceMajorityWriteLatency)
            ? serverStatus.metrics.repl.buffer.apply.count
            : serverStatus.metrics.repl.buffer.count;
        var bufferCountChange = bufferCount - bufferCountBefore;
        jsTestLog('Number of operations buffered on secondary since stopping applier: ' +
                  bufferCountChange);
        return bufferCountChange == numDocuments - 1;
    },
    'secondary did not buffer operations for new inserts on primary',
    ReplSetTest.kDefaultTimeoutMS,
    1000);

reconnect(secondary);
replSet.stepUp(secondary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});

// Secondary doesn't allow writes yet.
var res = secondary.getDB("admin").runCommand({"hello": 1});
assert(!res.isWritablePrimary);

// Prevent the current primary from stepping down
jsTest.log("disallowing heartbeat stepdown " + secondary.host);
var blockHeartbeatStepdownFailPoint = configureFailPoint(secondary, "blockHeartbeatStepdown");
jsTestLog("Shut down the rest of the set so the primary-elect has to step down");
replSet.stop(primary);
disableFailPoint(replSet.nodes[2]);  // Fail point needs to be off when node is shut down.
replSet.stop(2);

jsTestLog("Waiting for secondary to begin stepping down while in drain mode");
blockHeartbeatStepdownFailPoint.wait();

// Disable fail point to allow replication and allow secondary to finish drain mode while in the
// process of stepping down.
jsTestLog("Re-enabling replication on secondary");
assert.gt(numDocuments, secondary.getDB("foo").foo.find().itcount());
disableFailPoint(secondary);

// The node should now be able to apply the writes in its buffer.
jsTestLog("Waiting for node to drain its apply buffer");
assert.soon(function() {
    return secondary.getDB("foo").foo.find().itcount() == numDocuments;
});

jsTestLog("Checking that node is PRIMARY but not writable");
assert.eq(ReplSetTest.State.PRIMARY, secondary.adminCommand({replSetGetStatus: 1}).myState);
assert(!secondary.adminCommand('hello').isWritablePrimary);

jsTest.log("allowing heartbeat stepdown " + secondary.host);
blockHeartbeatStepdownFailPoint.off();

jsTestLog("Checking that node successfully stepped down");
replSet.waitForState(secondary, ReplSetTest.State.SECONDARY);
assert(!secondary.adminCommand('hello').isWritablePrimary);

// Now ensure that the node can successfully become primary again.
replSet.restart(0);
replSet.restart(2);
replSet.stepUp(secondary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});

assert.soon(function() {
    return secondary.adminCommand('hello').isWritablePrimary;
});

jsTestLog('Ensure new primary is writable.');
assert.commandWorked(secondary.getDB("foo").flag.insert({sentinel: 1}, {writeConcern: {w: 3}}));
// Check that no writes were lost.
assert.eq(secondary.getDB("foo").foo.find().itcount(), numDocuments);
replSet.stopSet();
