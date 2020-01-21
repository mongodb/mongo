/*
 * Test that replSetReconfig ignores the term value provided by a user.
 *
 * @tags: [requires_fcv_44]
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
// TODO SERVER-45408: uncomment once we enable serialization of the term field.
// assert.eq(config.term, 1);
replTest.stopSet();
}());