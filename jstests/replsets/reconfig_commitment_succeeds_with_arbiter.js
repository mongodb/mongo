/**
 * Verify that a non force replica set reconfig can be committed by a primary and arbiter, with a
 * secondary down.
 */
(function() {
"use strict";

// Make the secondary unelectable.
let rst =
    new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {arbiterOnly: true}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

jsTestLog("Shut down secondary.");

rst.stop(secondary);

jsTestLog("Safe reconfig twice to prove reconfigs are committed with secondary down.");

var config = rst.getReplSetConfigFromNode();

for (let i = 0; i < 2; i++) {
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
}

rst.restart(secondary);
rst.awaitReplication();
rst.stopSet();
}());
