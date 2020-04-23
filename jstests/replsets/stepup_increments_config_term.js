/**
 * Test that step-up increments the config term via reconfig.
 */
(function() {
'use strict';

load("jstests/replsets/rslib.js");

var name = 'stepup_increments_config_term';
var replTest = new ReplSetTest({name: name, nodes: 3, settings: {chainingAllowed: false}});

replTest.startSet();
replTest.initiate();

var primary = replTest.getPrimary();

// The original config should have the valid "version" and "term" fields.
const originalConfig = replTest.getReplSetConfigFromNode();
assert.gt(originalConfig.version, 0);
assert.gt(originalConfig.term, -1);

replTest.awaitReplication();
jsTestLog("Trying to step down primary.");
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60}));

jsTestLog("Waiting for PRIMARY(" + primary.host + ") to step down & become SECONDARY.");
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// Wait until the config has propagated to the secondary and the primary has learned of it, so that
// the config replication check is satisfied.
assert.soon(() => isConfigCommitted(replTest.getPrimary()));

const config = replTest.getReplSetConfigFromNode();
const msg = "Original config: " + tojson(originalConfig) + " current config: " + tojson(config);

// Stepup runs a reconfig with the same config version but higher config term.
assert.eq(originalConfig.version, config.version, msg);
assert.eq(originalConfig.term + 1, config.term, msg);

replTest.stopSet();
}());
