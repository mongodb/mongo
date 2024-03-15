// Test that the stepdown command can be run successfully during drain mode

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
    // Wait for Oplog Applier to hang on the failpoint.
    checkLog.contains(node,
                      "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");
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

assert.commandWorked(secondary.adminCommand({replSetStepDown: 60, force: true}));

// Assert stepdown was successful.
assert.eq(ReplSetTest.State.SECONDARY, secondary.adminCommand({replSetGetStatus: 1}).myState);
assert(!secondary.adminCommand('hello').isWritablePrimary);

// Prevent the producer from fetching new ops
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: 'stopReplProducer', mode: 'alwaysOn'}));

// Allow the secondary to apply the ops already in its buffer.
jsTestLog("Re-enabling replication on secondaries");
assert.gt(numDocuments, secondary.getDB("foo").foo.find().itcount());
secondaries.forEach(disableFailPoint);

// The node should now be able to apply the writes in its buffer.
jsTestLog("Waiting for node to drain its apply buffer");
assert.soon(function() {
    return secondary.getDB("foo").foo.find().itcount() == numDocuments;
});
replSet.stopSet();
