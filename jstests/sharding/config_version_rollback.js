/**
 * Tests that if the config.version document on a config server is rolled back, that config server
 * will detect the new config.version document when it gets recreated.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    var configRS = new ReplSetTest({nodes: 3});
    var nodes = configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});

    // Prevent any replication from happening, so that the initial writes that the config
    // server performs on first transition to primary can be rolled back.
    nodes.forEach(function(node) {
        assert.commandWorked(node.getDB('admin').runCommand(
            {configureFailPoint: 'stopOplogFetcher', mode: 'alwaysOn'}));
    });

    configRS.initiate();

    var origPriConn = configRS.getPrimary();
    var secondaries = configRS.getSecondaries();

    jsTest.log("Confirming that the primary has the config.version doc but the secondaries do not");
    var origConfigVersionDoc = origPriConn.getCollection('config.version').findOne();
    assert.neq(null, origConfigVersionDoc);
    secondaries.forEach(function(secondary) {
        secondary.setSlaveOk();
        assert.eq(null, secondary.getCollection('config.version').findOne());
    });

    // Ensure manually deleting the config.version document is not allowed.
    assert.writeErrorWithCode(origPriConn.getCollection('config.version').remove({}), 40302);
    assert.commandFailedWithCode(origPriConn.getDB('config').runCommand({drop: 'version'}), 40303);

    jsTest.log("Stepping down original primary");
    try {
        origPriConn.adminCommand({replSetStepDown: 60, force: true});
    } catch (x) {
        // replSetStepDown closes all connections, thus a network exception is expected here.
    }

    jsTest.log("Waiting for new primary to be elected and write a new config.version document");
    var newPriConn = configRS.getPrimary();
    assert.neq(newPriConn, origPriConn);

    var newConfigVersionDoc = newPriConn.getCollection('config.version').findOne();
    assert.neq(null, newConfigVersionDoc);
    assert.neq(origConfigVersionDoc.clusterId, newConfigVersionDoc.clusterId);

    jsTest.log("Re-enabling replication on all nodes");
    nodes.forEach(function(node) {
        assert.commandWorked(
            node.getDB('admin').runCommand({configureFailPoint: 'stopOplogFetcher', mode: 'off'}));
    });

    jsTest.log(
        "Waiting for original primary to rollback and replicate new config.version document");
    origPriConn.setSlaveOk();
    assert.soonNoExcept(function() {
        var foundClusterId = origPriConn.getCollection('config.version').findOne().clusterId;
        return friendlyEqual(newConfigVersionDoc.clusterId, foundClusterId);
    });

    jsTest.log("Forcing original primary to step back up and become primary again.");

    // Do prep work to make original primary transtion to primary again smoother by
    // waiting for all nodes to catch up to make them eligible to become primary and
    // step down the current primary to make it stop generating new oplog entries.
    configRS.awaitReplication(60 * 1000);

    try {
        newPriConn.adminCommand({replSetStepDown: 60, force: true});
    } catch (x) {
        // replSetStepDown closes all connections, thus a network exception is expected here.
    }

    // Ensure former primary is eligible to become primary once more.
    assert.commandWorked(origPriConn.adminCommand({replSetFreeze: 0}));

    // Keep on trying until this node becomes the primary. One reason it can fail is when the other
    // nodes have newer oplog entries and will thus refuse to vote for this node.
    assert.soon(function() {
        return (origPriConn.adminCommand({replSetStepUp: 1})).ok;
    });

    assert.soon(function() {
        return origPriConn == configRS.getPrimary();
    });

    // Now we just need to start up a mongos and add a shard to confirm that the shard gets added
    // with the proper clusterId value.
    jsTest.log("Starting mongos");
    var mongos = MongoRunner.runMongos({configdb: configRS.getURL()});

    jsTest.log("Starting shard mongod");
    var shard = MongoRunner.runMongod({shardsvr: ""});

    jsTest.log("Adding shard to cluster");
    assert.commandWorked(mongos.adminCommand({addShard: shard.host}));

    jsTest.log("Verifying that shard was provided the proper clusterId");
    var shardIdentityDoc = shard.getDB('admin').system.version.findOne({_id: 'shardIdentity'});
    printjson(shardIdentityDoc);
    assert.eq(newConfigVersionDoc.clusterId,
              shardIdentityDoc.clusterId,
              "oldPriClusterId: " + origConfigVersionDoc.clusterId);
    configRS.stopSet();
})();
