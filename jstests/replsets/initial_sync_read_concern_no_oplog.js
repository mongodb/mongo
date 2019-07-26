// Test that if an afterClusterTime query is issued to a node in initial sync that has not yet
// created its oplog, the node returns an error rather than crashing.
(function() {
'use strict';
load('jstests/libs/check_log.js');

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const secondary = replSet.add();

assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCreatingOplog', mode: 'alwaysOn'}));
replSet.reInitiate();

checkLog.contains(secondary,
                  'initial sync - initialSyncHangBeforeCreatingOplog fail point enabled');

assert.commandFailedWithCode(
    secondary.getDB('local').runCommand(
        {find: 'coll', limit: 1, readConcern: {afterClusterTime: Timestamp(1, 1)}}),
    ErrorCodes.NotYetInitialized);

assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCreatingOplog', mode: 'off'}));

replSet.awaitReplication();
replSet.awaitSecondaryNodes();

replSet.stopSet();
})();