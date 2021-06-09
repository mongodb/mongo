// Tests reconfigure with hidden.
load("jstests/replsets/rslib.js");

(function() {
"use strict";

var replTest = new ReplSetTest({name: 'testSet', nodes: 3});
replTest.startSet();
replTest.initiate();

print("replset5.js reconfigure with priority=0");
var config = replTest.getReplSetConfigFromNode();
config.version++;
config.settings = {};
config.settings.heartbeatTimeoutSecs = 15;
// Prevent node 2 from becoming primary, as we will attempt to set it to hidden later.
config.members[2].priority = 0;
reconfig(replTest, config);

print("replset5.js reconfigure with hidden=1");
var primary = replTest.getPrimary();
config = primary.getDB("local").system.replset.findOne();

assert.eq(15, config.settings.heartbeatTimeoutSecs);

config.version++;
config.members[2].hidden = 1;

primary = reconfig(replTest, config);

config = primary.getSiblingDB("local").system.replset.findOne();
assert.eq(config.members[2].hidden, true);

replTest.stopSet();
}());
