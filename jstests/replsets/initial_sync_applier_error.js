/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source and fetches operations from sync source; and
 * 3) applies operations from the source after op_start1.
 *
 * This test renames a collection on the source between phases 1 and 2, but renameCollection is not
 * supported in initial sync. The secondary will initially fail to apply the command in phase 3
 * and subsequently have to retry the initial sync.
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

var name = 'initial_sync_applier_error';
var replSet = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {arbiterOnly: true}}],
});

replSet.startSet();
replSet.initiate();
var primary = replSet.getPrimary();

var coll = primary.getDB('test').getCollection(name);
assert.commandWorked(coll.insert({_id: 0, content: "hi"}));

// Add a secondary node but make it hang after retrieving the last op on the source
// but before copying databases.
var secondary =
    replSet.add({setParameter: "numInitialSyncAttempts=2", rsConfig: {votes: 0, priority: 0}});
secondary.setSecondaryOk();

let failPoint = configureFailPoint(secondary, 'initialSyncHangBeforeCopyingDatabases');
replSet.reInitiate();

// Wait for fail point message to be logged.
failPoint.wait();

var newCollName = name + '_2';
assert.commandWorked(coll.renameCollection(newCollName, true));
failPoint.off();

checkLog.contains(secondary, 'Initial sync done');

replSet.awaitReplication();
replSet.awaitSecondaryNodes();

assert.eq(0, secondary.getDB('test').getCollection(name).count());
assert.eq(1, secondary.getDB('test').getCollection(newCollName).count());
assert.eq("hi", secondary.getDB('test').getCollection(newCollName).findOne({_id: 0}).content);
replSet.stopSet();
})();
