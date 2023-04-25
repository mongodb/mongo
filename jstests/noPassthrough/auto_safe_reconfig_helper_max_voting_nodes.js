/**
 * Test that the 'reconfig' helper function correctly executes reconfigs between configs that have
 * the maximum number of allowed voting nodes.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/replsets/rslib.js");

// Make secondaries unelectable. Add 7 voting nodes, which is the maximum allowed.
const replTest = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0, votes: 0}}
    ]
});
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
let [m0, m1, m2, m3, m4, m5, m6, m7] = origConfig.members;

//
// Test max voting constraint.
//

jsTestLog("Test max voting constraint.");

// Test making one node non voting and the other voting.
m6.votes = 0;
m6.priority = 0;
m7.votes = 1;
m7.priority = 1;
config.members = [m0, m1, m2, m3, m4, m5, m6, m7];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// And test switching the vote back.
m6.votes = 1;
m6.priority = 0;
m7.votes = 0;
m7.priority = 0;
config.members = [m0, m1, m2, m3, m4, m5, m6, m7];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Test swapping out a voting member.
m6.votes = 1;
m6.priority = 0;
config.members = [m0, m1, m2, m3, m4, m5, m6];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

m7.votes = 1;
m7.priority = 1;
config.members = [m0, m1, m2, m3, m4, m5, m7];
reconfig(replTest, config);
assertSameConfigContent(replTest.getReplSetConfigFromNode(), config);

// Restore the original config before shutting down.
m7.votes = 0;
m7.priority = 0;
config.members = [m0, m1, m2, m3, m4, m5, m6, m7];
reconfig(replTest, config);
// There is a chance that some nodes haven't finished reconfig, if we directly call stopSet, those
// nodes may fail to answer certain commands and fail the test.
waitAllNodesHaveConfig(replTest, config);
replTest.stopSet();
})();
