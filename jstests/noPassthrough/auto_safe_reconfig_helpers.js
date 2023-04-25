/**
 * Test that the 'reconfig' helper function correctly executes arbitrary reconfigs.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/replsets/rslib.js");

// Make secondaries unelectable.
const replTest =
    new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
replTest.startSet();
let conf = replTest.getReplSetConfig();
conf.settings = {
    // Speed up config propagation.
    heartbeatIntervalMillis: 100,
};
replTest.initiate(conf);

// Start out with config {n0,n1,n2}
let config = replTest.getReplSetConfigFromNode();
let origConfig = Object.assign({}, config);
let [m0, m1, m2] = origConfig.members;

//
// Test reconfigs that only change config settings but not the member set.
//

jsTestLog("Testing reconfigs that don't modify the member set.");

// Change the 'electionTimeoutMillis' setting.
config.settings.electionTimeoutMillis = config.settings.electionTimeoutMillis + 1;
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Do a reconfig that leaves out a config setting that will take on a default.
delete config.settings.electionTimeoutMillis;
reconfig(replTest, config);
// The installed config should be the same as the given config except for the default value.
let actualConfig = replTest.getReplSetConfigFromNode();
assert(actualConfig.settings.hasOwnProperty("electionTimeoutMillis"));
config.settings.electionTimeoutMillis = actualConfig.settings.electionTimeoutMillis;
assertSameConfigContent(actualConfig, config);

// Change a member config parameter.
config.members[0].priority = 2;
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

//
// Test member set changes.
//

jsTestLog("Testing member set changes.");

// Start in the original config and reset the config object.
reconfig(replTest, origConfig);
config = replTest.getReplSetConfigFromNode();

// Remove 2 nodes, {n1, n2}.
config.members = [m0];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Add 2 nodes, {n1, n2}.
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Remove one node so we can test swapping a node out.
config.members = [m0, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Remove n2 and add n1 simultaneously (swap a node).
config.members = [m0, m1];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Remove both existing nodes (n0, n1) and add a new node (n2). Removing a node that is executing
// the reconfig shouldn't be allowed, but we test it here to make sure it fails in an expected way.
m2.priority = 1;
config.members = [m2];
try {
    reconfig(replTest, config);
} catch (e) {
    assert.eq(e.code, ErrorCodes.NewReplicaSetConfigurationIncompatible, tojson(e));
}

// Reset the member's priority.
m2.priority = 0;

//
// Test voting set changes that don't change the member set.
//

jsTestLog("Testing voting set changes.");

// Start in the original config.
reconfig(replTest, origConfig);

// Remove two nodes, {n1,n2}, from the voting set.
m1.votes = 0;
m2.votes = 0;
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Add two nodes, {n1,n2}, to the voting set.
m1.votes = 1;
m2.votes = 1;
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Remove one node n1 from the voting set.
m1.votes = 0;
m2.votes = 1;
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Add one node (n1) and remove one node (n2) from the voting set.
m1.votes = 1;
m2.votes = 0;
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Make n2 voting by omitting a 'votes' field, which is allowed.
delete m2.votes;
config.members = [m0, m1, m2];
reconfig(replTest, config);
actualConfig = replTest.getReplSetConfigFromNode();
assert.eq(actualConfig.members[2].votes, 1);
config.members[2].votes = 1;
assertSameConfigContent(actualConfig, config);

// Remove the primary (n0) from the voting set and remove n2. We expect this to fail.
m0.votes = 0;
m0.priority = 0;
m1.priority = 1;
m1.votes = 1;
m2.priority = 0;
m2.votes = 0;
config.members = [m0, m1, m2];
try {
    reconfig(replTest, config);
} catch (e) {
    assert.eq(e.code, ErrorCodes.NewReplicaSetConfigurationIncompatible, tojson(e));
}

//
// Test simultaneous voting set and member set changes.
//

jsTestLog("Testing simultaneous voting set and member set changes.");

// Start in the original config and reset vote counts.
m0.votes = 1;
m0.priority = 1;
m1.votes = 1;
m1.priority = 0;
m2.votes = 1;
m2.priority = 0;
reconfig(replTest, origConfig);

// Remove voting node n2 and make n1 non voting.
m1.votes = 0;
m2.votes = 1;
config.members = [m0, m1];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Add voting node n2 and make n1 voting.
m1.votes = 1;
m2.votes = 1;
config.members = [m0, m1, m2];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Restore the original config before shutting down.
reconfig(replTest, origConfig);
// There is a chance that some nodes haven't finished reconfig, if we directly call stopSet, those
// nodes may fail to answer certain commands and fail the test.
waitAllNodesHaveConfig(replTest, config);
replTest.stopSet();
})();
