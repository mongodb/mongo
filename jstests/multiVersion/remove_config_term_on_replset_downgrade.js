/**
 * Test the downgrade of a replica set from latest version to last-stable version succeeds and
 * removes the "term" field of config document.
 */

(function() {
'use strict';

load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/libs/test_background_ops.js');

const newVersion = "latest";
const oldVersion = "last-stable";

function testDowngrade(useArbiter) {
    let nodes = {
        n1: {binVersion: newVersion},
        n2: {binVersion: newVersion},
        n3: {binVersion: newVersion}
    };

    let replicaSetName = `testDowngrade${useArbiter ? "With" : "Without"}Arbiter`;
    let rst = new ReplSetTest({name: replicaSetName, nodes: nodes});
    let nodenames = rst.nodeList();
    rst.startSet();
    rst.initiate({
        "_id": replicaSetName,
        "members": [
            {"_id": 0, "host": nodenames[0]},
            {"_id": 1, "host": nodenames[1]},
            {"_id": 2, "host": nodenames[2], arbiterOnly: useArbiter}
        ]
    });

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
        jsTestLog("Checking the config term on node " + tojson(node.host) +
                  " after FCV downgrade.");
        let config = node.getDB("local").getCollection("system.replset").findOne();
        assert(!config.hasOwnProperty("term"), tojson(config));
        // The configs can only differ in config versions and terms.
        config.term = configInNewVersion.term;
        // The versions differ because of the force reconfig on downgrade.
        assert.eq(config.version, configInNewVersion.version + 1);
        config.version = configInNewVersion.version;
        assert.docEq(configInNewVersion, config);
    });

    jsTest.log("Downgrading replica set..");
    rst.upgradeSet({binVersion: oldVersion});
    jsTest.log("Downgrade complete.");

    // Check that the term field doesn't exist in the config document on all nodes.
    rst.nodes.forEach(function(node) {
        reconnect(node);
        jsTestLog("Checking the config term on node " + tojson(node.host) +
                  " after binary downgrade.");
        let config = node.getDB("local").getCollection("system.replset").findOne();
        assert(!config.hasOwnProperty("term"), tojson(config));
        // The configs can only differ in config versions and terms.
        config.term = configInNewVersion.term;
        // The versions differ because of the force reconfig on downgrade.
        assert.eq(config.version, configInNewVersion.version + 1);
        config.version = configInNewVersion.version;
        assert.docEq(configInNewVersion, config);
    });
    rst.stopSet();
}

jsTestLog('Test downgrading WITHOUT an arbiter');
testDowngrade(false);
jsTestLog('Test downgrading WITH an arbiter');
testDowngrade(true);
})();
