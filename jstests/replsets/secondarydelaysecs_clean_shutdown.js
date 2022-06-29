// SERVER-21118 don't hang at shutdown or apply ops too soon with secondaryDelaySecs.
//
// @tags: [
//   requires_persistence,
// ]
load('jstests/replsets/rslib.js');
(function() {
"use strict";

// Skip db hash check since secondary has slave delay.
TestData.skipCheckDBHashes = true;

var ns = "test.coll";

var rst = new ReplSetTest({
    nodes: 2,
});

var conf = rst.getReplSetConfig();
conf.members[1].votes = 0;
conf.members[1].priority = 0;
conf.members[1].hidden = true;
conf.members[1].secondaryDelaySecs = 0;  // Set later.

rst.startSet();
rst.initiate(conf);

var primary = rst.getPrimary();  // Waits for PRIMARY state.

// Push some ops through before setting slave delay.
assert.commandWorked(primary.getCollection(ns).insert([{}, {}, {}], {writeConcern: {w: 2}}));

// Set the delay field and wait for secondary to receive the change.
conf = rst.getReplSetConfigFromNode();
conf.version++;
conf.members[1].secondaryDelaySecs = 24 * 60 * 60;
reconfig(rst, conf);
assert.soon(() => rst.getReplSetConfigFromNode(1).members[1].secondaryDelaySecs > 0,
            () => rst.getReplSetConfigFromNode(1));

// The secondary apply loop only checks for the delay field changes once per second.
sleep(2000);
var secondary = rst.getSecondary();
const lastOp = getLatestOp(secondary);

assert.commandWorked(primary.getCollection(ns).insert([{}, {}, {}]));
assert.soon(() => secondary.adminCommand('serverStatus').metrics.repl.buffer.count > 0,
            () => secondary.adminCommand('serverStatus').metrics.repl);
assert.neq(getLatestOp(primary), lastOp);
assert.eq(getLatestOp(secondary), lastOp);

sleep(2000);  // Prevent the test from passing by chance.
assert.eq(getLatestOp(secondary), lastOp);

// Make sure shutdown won't take a long time due to I/O.
secondary.adminCommand('fsync');

// Shutting down shouldn't take long, but in case of a slow machine closing/opening of wiredTiger on
// shutdown for reconfiguration takes extra time hence the two minutes wait.
assert.lt(Date.timeFunc(() => rst.stop(1)), 2 * 60 * 1000);

secondary = rst.restart(1);
rst.awaitSecondaryNodes();

assert.eq(getLatestOp(secondary), lastOp);
sleep(2000);  // Prevent the test from passing by chance.
assert.eq(getLatestOp(secondary), lastOp);

rst.stopSet();
})();
