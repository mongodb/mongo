/**
 * Test that replSetReconfig waits for a majority of nodes to replicate the config
 * before starting another reconfig.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isConfigCommitted} from "jstests/replsets/rslib.js";

let replTest = new ReplSetTest({nodes: 2, useBridge: true});
replTest.startSet();
// Initiating with a high election timeout prevents unnecessary elections and also prevents
// the primary from stepping down if it cannot communicate with the secondary.
replTest.initiate();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

// Disconnect the secondary from the primary.
secondary.disconnect(primary);

// Configure a failpoint so that we bypass the config quorum check and go straight to the
// config replication check.
let reconfigFailPoint = configureFailPoint(primary, "omitConfigQuorumCheck");

// Run a reconfig with a timeout of 5 seconds, this should fail with a maxTimeMSExpired error.
var config = primary.getDB("local").system.replset.findOne();
config.version++;
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
    ErrorCodes.MaxTimeMSExpired,
);
assert.eq(isConfigCommitted(primary), false);

// Try to run another reconfig, which should also time out because the previous config is
// not committed.
var config = primary.getDB("local").system.replset.findOne();
config.version++;
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
    ErrorCodes.CurrentConfigNotCommittedYet,
);
assert.eq(isConfigCommitted(primary), false);

// Reconnect the secondary to the primary.
secondary.reconnect(primary);

// Reconfig should now succeed.
config.version++;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
assert.soon(() => isConfigCommitted(primary));

reconfigFailPoint.off();

replTest.stopSet();
