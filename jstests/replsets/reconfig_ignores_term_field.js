/**
 * Test that replSetReconfig ignores the term value provided by a user.
 */

(function() {

// Start a 2 node replica set where one of the nodes has priority 0 to
// prevent unnecessary elections.
var replTest = new ReplSetTest({nodes: 1});
var nodes = replTest.startSet();
replTest.initiate();

// After the first election, the term should be 1.
var primary = replTest.getPrimary();

jsTestLog("Reconfig command ignores user provided term, 50");
var config = primary.getDB("local").system.replset.findOne();
printjson(config);
config.version++;
config.members[nodes.indexOf(primary)].priority = 1;  // Legal reconfig.
config.term = 50;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
replTest.awaitReplication();

config = primary.getDB("local").system.replset.findOne();
assert.eq(config.term, 1);

jsTestLog("Force reconfig ignores user provided term");
config.term = 55;
config.version++;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config, force: true}));
config = primary.getDB("local").system.replset.findOne();
// Force reconfig sets the config term to -1. During config
// serialization, a -1 term is treated as a missing field.
assert(!config.hasOwnProperty("term"));

jsTestLog("Force reconfig with missing term results in term -1");
delete config.term;
config.version++;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config, force: true}));
config = primary.getDB("local").system.replset.findOne();
// Force reconfig sets the config term to -1. During config
// serialization, a -1 term is treated as a missing field.
assert(!config.hasOwnProperty("term"));

replTest.stopSet();
}());
