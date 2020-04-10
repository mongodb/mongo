/**
 * Test the upgrade of a replica set from last-stable to latest version succeeds and adds the "term"
 * field of the config document.
 */

(function() {
'use strict';

load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/libs/test_background_ops.js');

const newVersion = "latest";
const oldVersion = "last-stable";

let nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion}
};
let replicaSetName = 'add_config_term_on_replset_upgrade';
let rst = new ReplSetTest({name: replicaSetName, nodes: nodes});
let nodenames = rst.nodeList();
rst.startSet();
rst.initiate({
    "_id": replicaSetName,
    "members": [
        {"_id": 0, "host": nodenames[0]},
        {"_id": 1, "host": nodenames[1]},
        {"_id": 2, "host": nodenames[2]}
    ]
});

// Upgrade the set.
jsTest.log("Upgrading replica set..");
rst.upgradeSet({binVersion: newVersion});
jsTest.log("Upgrade complete.");

let config = rst.getPrimary().getDB("local").getCollection("system.replset").findOne();
assert(!config.hasOwnProperty("term"));

let configInOldVersion = rst.getReplSetConfigFromNode();

jsTest.log("Upgrading FCV to 4.4");
let primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Check that the term field exists in the config document on all nodes. The term of the config
// should match the node's current term.
rst.nodes.forEach(function(node) {
    reconnect(node);
    jsTestLog("Checking the config term on node " + tojson(node.host) + " after binary upgrade.");
    let config = node.getDB("local").getCollection("system.replset").findOne();
    let currentTerm = node.adminCommand({replSetGetStatus: 1}).term;
    // Config should have a term field that matches the node's current term.
    assert(config.hasOwnProperty("term"), tojson(config));
    assert.eq(config.term, currentTerm, tojson(config));
    // The configs can only differ in config versions and terms. Versions differ because of the
    // force reconfig on upgrade.
    configInOldVersion.term = config.term;
    assert.eq(config.version, configInOldVersion.version + 1);
    config.version = configInOldVersion.version;
    assert.docEq(configInOldVersion, config);
});

rst.stopSet();
})();
