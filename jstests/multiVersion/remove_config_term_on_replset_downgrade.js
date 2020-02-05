/**
 * Test the downgrade of a replica set from latest version to last-stable version succeeds and
 * removes the "term" field of config document.
 */

(function() {
'use strict';

load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/libs/test_background_ops.js');

let newVersion = "latest";
let oldVersion = "last-stable";

let nodes = {
    n1: {binVersion: newVersion},
    n2: {binVersion: newVersion},
    n3: {binVersion: newVersion}
};

let rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

// The default FCV is latestFCV for non-shard replica sets.
let primaryAdminDB = rst.getPrimary().getDB("admin");
checkFCV(primaryAdminDB, latestFCV);

// Reconfig in FCV 4.4.
let originalConfig = rst.getReplSetConfigFromNode();
originalConfig.version++;
reconfig(rst, originalConfig);
rst.awaitNodesAgreeOnConfigVersion();

// Check that the term field exists in the config document on all nodes.
rst.nodes.forEach(function(node) {
    jsTestLog("Checking the config term on node " + tojson(node.host) + " before downgrade.");
    let config = node.getDB("local").getCollection("system.replset").findOne();
    assert(config.hasOwnProperty("term"));
});

// Remember the config from the primary before the downgrade.
let configInNewVersion = rst.getReplSetConfigFromNode();

jsTest.log("Downgrading FCV to 4.2");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
rst.awaitReplication();
// Check that the term field doesn't exist in the config document on all nodes.
rst.nodes.forEach(function(node) {
    jsTestLog("Checking the config term on node " + tojson(node.host) + " after FCV downgrade.");
    let config = node.getDB("local").getCollection("system.replset").findOne();
    assert(!config.hasOwnProperty("term"), tojson(config));
    config.term = configInNewVersion.term;
    assert.docEq(configInNewVersion, config);
});

jsTest.log("Downgrading replica set..");
rst.upgradeSet({binVersion: oldVersion});
jsTest.log("Downgrade complete.");

// Check that the term field doesn't exist in the config document on all nodes.
rst.nodes.forEach(function(node) {
    reconnect(node);
    jsTestLog("Checking the config term on node " + tojson(node.host) + " after binary downgrade.");
    let config = node.getDB("local").getCollection("system.replset").findOne();
    assert(!config.hasOwnProperty("term"), tojson(config));
    config.term = configInNewVersion.term;
    assert.docEq(configInNewVersion, config);
});
rst.stopSet();
})();
