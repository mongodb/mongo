/**
 * Verify that a non force replica set reconfig can be committed by a primary and arbiter, with a
 * secondary down.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Make the secondary unelectable.
let rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {arbiterOnly: true}}],
    nodeOptions: {
        setParameter: {
            // On in-memory variants, restarting a node triggers initial sync. Waiting for the
            // sync source's lastStableRecoveryTimestamp to advance during initial sync can hang
            // this test, so we disable that wait. See SERVER-128221 for more information.
            "initialSyncWaitForSyncSourceLastStableRecoveryTs": false,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

jsTestLog("Shut down secondary.");

rst.stop(secondary);

jsTestLog("Safe reconfig twice to prove reconfigs are committed with secondary down.");

let config = rst.getReplSetConfigFromNode();

for (let i = 0; i < 2; i++) {
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
}

rst.restart(secondary);
rst.awaitReplication();
rst.stopSet();
