// Test drain mode when transitioning to PRIMARY
// 1. Set up a 3-node set.
// 2. Prevent applying retrieved ops on the SECONDARY.
// 3. Insert data to ensure the SECONDARY has ops to apply in its queue.
// 4. Shutdown PRIMARY.
// 5. Wait for SECONDARY to become PRIMARY.
// 6. Confirm that the new PRIMARY cannot accept writes while in drain mode.
// 6a. Confirm that the new PRIMARY cannot accept reads while in drain mode.
// 7. Enable applying ops.
// 8. Ensure the ops in queue are applied and that the PRIMARY begins to accept writes as usual.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
var nodes = replSet.nodeList();
replSet.startSet();
replSet.initiate({
    "_id": "testSet",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true}
    ],
    // No primary catch-up so we focus on the drain mode.
    "settings": {"catchUpTimeoutMillis": 0},
});

var primary = replSet.getPrimary();
var secondary = replSet.getSecondary();

// The default WC is majority and rsSyncApplyStop failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Do an initial insert to prevent the secondary from going into recovery
var numDocuments = 20;
var bulk = primary.getDB("foo").foo.initializeUnorderedBulkOp();
var bigString = Array(1024 * 1024).toString();
assert.commandWorked(primary.getDB("foo").foo.insert({big: bigString}));
replSet.awaitReplication();
assert.commandWorked(
    secondary.getDB("admin").runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}),
    'failed to enable fail point on secondary');

const reduceMajorityWriteLatency =
    FeatureFlagUtil.isPresentAndEnabled(secondary, "ReduceMajorityWriteLatency");
var bufferCountBefore = (reduceMajorityWriteLatency)
    ? secondary.getDB('foo').serverStatus().metrics.repl.buffer.apply.count
    : secondary.getDB('foo').serverStatus().metrics.repl.buffer.count;
for (var i = 1; i < numDocuments; ++i) {
    bulk.insert({big: bigString});
}
assert.commandWorked(bulk.execute());
jsTestLog('Number of documents inserted into collection on primary: ' + numDocuments);
assert.eq(numDocuments, primary.getDB("foo").foo.find().itcount());

assert.soon(function() {
    var serverStatus = secondary.getDB('foo').serverStatus();
    var bufferCount = (reduceMajorityWriteLatency) ? serverStatus.metrics.repl.buffer.apply.count
                                                   : serverStatus.metrics.repl.buffer.count;
    var bufferCountChange = bufferCount - bufferCountBefore;
    jsTestLog('Number of operations buffered on secondary since stopping applier: ' +
              bufferCountChange);
    return bufferCountChange >= numDocuments - 1;
}, 'secondary did not buffer operations for new inserts on primary', 300000, 1000);

// Kill primary; secondary will enter drain mode to catch up
primary.getDB("admin").shutdownServer({force: true});

replSet.waitForState(secondary, ReplSetTest.State.PRIMARY);

// Ensure new primary is not yet writable
jsTestLog('New primary should not be writable yet');
assert.writeError(secondary.getDB("foo").flag.insert({sentinel: 2}));
assert(!secondary.getDB("admin").runCommand({"hello": 1}).isWritablePrimary);

// Ensure new primary is not yet readable without secondaryOk bit.
secondary.setSecondaryOk(false);
jsTestLog('New primary should not be readable yet, without secondaryOk bit');
var res = secondary.getDB("foo").runCommand({find: "foo"});
assert.commandFailed(res);
assert.eq(ErrorCodes.NotPrimaryNoSecondaryOk,
          res.code,
          "find failed with unexpected error code: " + tojson(res));
// Nor should it be readable with the secondaryOk bit.
secondary.setSecondaryOk();
assert.commandWorked(secondary.getDB("foo").runCommand({find: "foo"}));

assert(!secondary.adminCommand({"hello": 1}).isWritablePrimary);

// Allow draining to complete
jsTestLog('Disabling fail point on new primary to allow draining to complete');
assert.commandWorked(
    secondary.getDB("admin").runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}),
    'failed to disable fail point on new primary');
primary = replSet.getPrimary();

// Ensure new primary is writable
jsTestLog('New primary should be writable after draining is complete');
assert.commandWorked(primary.getDB("foo").flag.insert({sentinel: 1}));
// Check for at least two entries. There was one prior to freezing op application on the
// secondary and we cannot guarantee all writes reached the secondary's op queue prior to
// shutting down the original primary.
assert.gte(primary.getDB("foo").foo.find().itcount(), 2);
replSet.stopSet();
