
'use strict';

load("./jstests/multiVersion/libs/multi_rs.js");
load("./jstests/replsets/rslib.js");

/**
 * Test that starts up a replica set with 2 nodes of version 'replSetVersion', inserts some data,
 * then adds a new node to the replica set with version 'newNodeVersion' and waits for initial sync
 * to complete. If the 'fcv' argument is given, sets the feature compatibility version of the
 * replica set to 'fcv' before adding the third node.
 */
var multversionInitialSyncTest = function(
    name, replSetVersion, newNodeVersion, configSettings, fcv) {

    var nodes = {n1: {binVersion: replSetVersion}, n2: {binVersion: replSetVersion}};

    jsTestLog("Starting up a two-node '" + replSetVersion + "' version replica set.");
    var rst = new ReplSetTest({name: name, nodes: nodes});
    rst.startSet();

    var conf = rst.getReplSetConfig();
    conf.settings = configSettings;
    rst.initiate(conf);

    // Wait for a primary node.
    var primary = rst.getPrimary();

    // Set 'featureCompatibilityVersion' if given.
    if (fcv) {
        jsTestLog("Setting FCV to '" + fcv + "' on the primary.");
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: fcv}));
    }

    // Insert some data and wait for replication.
    for (var i = 0; i < 25; i++) {
        primary.getDB("foo").foo.insert({_id: i});
    }
    rst.awaitReplication();

    jsTestLog("Bringing up a new node with version '" + newNodeVersion + "' and adding to set.");
    rst.add({binVersion: newNodeVersion});
    rst.reInitiate();

    jsTestLog("Waiting for new node to be synced.");
    rst.awaitReplication();
    rst.awaitSecondaryNodes();

    rst.stopSet();
};