// Test that if an afterClusterTime query is issued to a node in initial sync that has not yet
// created its oplog, the node returns an error rather than crashing.
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const secondary = replSet.add({rsConfig: {votes: 0, priority: 0}});

const failPoint = configureFailPoint(secondary, 'initialSyncHangBeforeCreatingOplog');
replSet.reInitiate();

failPoint.wait();

assert.commandFailedWithCode(
    secondary.getDB('local').runCommand(
        {find: 'coll', limit: 1, readConcern: {afterClusterTime: Timestamp(1, 1)}}),
    ErrorCodes.NotYetInitialized);

failPoint.off();

replSet.awaitReplication();
replSet.awaitSecondaryNodes();

replSet.stopSet();
})();
