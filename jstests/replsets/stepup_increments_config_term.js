/**
 * Test that step-up increments the config term via reconfig.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isConfigCommitted} from "jstests/replsets/rslib.js";

let name = "stepup_increments_config_term";
let replTest = new ReplSetTest({name: name, nodes: 3, settings: {chainingAllowed: false}});

replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

// The original config should have the valid "version" and "term" fields.
const originalConfig = replTest.getReplSetConfigFromNode();
assert.gt(originalConfig.version, 0);
assert.gt(originalConfig.term, -1);

replTest.awaitReplication();
jsTestLog("Trying to step down primary.");
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60}));

jsTestLog("Waiting for PRIMARY(" + primary.host + ") to step down & become SECONDARY.");
replTest.awaitSecondaryNodes(null, [primary]);

// Wait until the config has propagated to the secondary and the primary has learned of it, so that
// the config replication check is satisfied.
assert.soon(() => isConfigCommitted(replTest.getPrimary()));

const config = replTest.getReplSetConfigFromNode();
const msg = "Original config: " + tojson(originalConfig) + " current config: " + tojson(config);

// Stepup runs a reconfig with the same config version but higher config term.
assert.eq(originalConfig.version, config.version, msg);
assert.eq(originalConfig.term + 1, config.term, msg);

replTest.stopSet();
