/**
 * Tests that rolling back the insertion of the shardIdentity document on a shard causes the node
 * rolling it back to shut down.
 * @tags: [requires_persistence, requires_journaling]
 */

(function() {
    "use strict";

    load('jstests/libs/write_concern_util.js');

    // This ShardingTest is only started to set up a config server replica set and include its
    // connection string in the shardIdentity doc.
    var st = new ShardingTest({shards: 1});

    var replTest = new ReplSetTest({nodes: 3});
    var nodes = replTest.startSet({shardsvr: ''});
    replTest.initiate();

    var priConn = replTest.getPrimary();
    var secondaries = replTest.getSecondaries();
    var configConnStr = st.configRS.getURL();

    // In general, shardsvrs default to the lower FCV on fresh start up (their FCV is set to the
    // cluster's FCV on addShard). However, this test starts a shardsvr that it never adds to the
    // cluster, but expects behavior that only a FCV>=4.0 shardsvr would execute:
    //
    // In FCV 3.6, clean shutdown writes uncommitted data to disk in the v3.6-compatible format.
    // When the node is restarted and sees uncommitted data in the v3.6-compatible format, it fails
    // to start up. A user can recover from this by restarting the node in v3.6 so that it uses the
    // rollback via refetch algorithm (rather than recoverable rollback), or by re-initial syncing
    // the node (by clearing its data directory).
    //
    // In FCV 4.0, clean shutdown does not write uncommitted data in the v3.6-compatible format, so
    // the node is able to be restarted.
    //
    // To avoid re-writing this test to take one of these user actions, we simply set the FCV on the
    // --shardsvr explicitly to 4.0.
    priConn.adminCommand({setFeatureCompatibilityVersion: "4.0"});

    // Wait for the secondaries to have the latest oplog entries before stopping the fetcher to
    // avoid the situation where one of the secondaries will not have an overlapping oplog with
    // the other nodes once the primary is killed.
    replTest.awaitSecondaryNodes();

    replTest.awaitReplication();

    stopServerReplication(secondaries);

    jsTest.log("inserting shardIdentity document to primary that shouldn't replicate");

    var shardIdentityDoc = {
        _id: 'shardIdentity',
        configsvrConnectionString: configConnStr,
        shardName: 'newShard',
        clusterId: ObjectId()
    };

    assert.writeOK(priConn.getDB('admin').system.version.update(
        {_id: 'shardIdentity'}, shardIdentityDoc, {upsert: true}));

    // Ensure sharding state on the primary was initialized
    var res = priConn.getDB('admin').runCommand({shardingState: 1});
    assert(res.enabled, tojson(res));
    assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);

    // Ensure sharding state on the secondaries was *not* initialized
    secondaries.forEach(function(secondary) {
        secondary.setSlaveOk(true);
        res = secondary.getDB('admin').runCommand({shardingState: 1});
        assert(!res.enabled, tojson(res));
    });

    // Ensure manually deleting the shardIdentity document is not allowed.
    assert.writeErrorWithCode(priConn.getDB('admin').system.version.remove({_id: 'shardIdentity'}),
                              40070);

    jsTest.log("shutting down primary");
    // Shut down the primary so a secondary gets elected that definitely won't have replicated the
    // shardIdentity insert, which should trigger a rollback on the original primary when it comes
    // back online.
    replTest.stop(priConn);

    // Disable the fail point so that the elected node can exit drain mode and finish becoming
    // primary.
    restartServerReplication(secondaries);

    // Wait for a new healthy primary
    var newPriConn = replTest.getPrimary();
    assert.neq(priConn, newPriConn);
    assert.writeOK(newPriConn.getDB('test').foo.insert({a: 1}, {writeConcern: {w: 'majority'}}));

    // Restart the original primary so it triggers a rollback of the shardIdentity insert. Pass
    // {waitForConnect : false} to avoid a race condition between the node crashing (which we
    // expect)
    // and waiting to be able to connect to the node.
    jsTest.log("Restarting original primary");
    priConn = replTest.start(priConn, {waitForConnect: false}, true);

    // Wait until we cannot create a connection to the former primary, which indicates that it must
    // have shut itself down during the rollback.
    jsTest.log("Waiting for original primary to rollback and shut down");
    // Wait until the node shuts itself down during the rollback. We will hit the first assertion if
    // we rollback using 'recoverToStableTimestamp' and the second if using 'rollbackViaRefetch'.
    assert.soon(() => {
        return (rawMongoProgramOutput().indexOf("Fatal Assertion 50712") !== -1 ||
                rawMongoProgramOutput().indexOf("Fatal Assertion 40498") !== -1);
    });

    // Restart the original primary again.  This time, the shardIdentity document should already be
    // rolled back, so there shouldn't be any rollback and the node should stay online.
    jsTest.log(
        "Restarting original primary a second time and waiting for it to successfully become " +
        "secondary");
    try {
        // Join() with the crashed mongod and ignore its bad exit status.
        MongoRunner.stopMongod(priConn);
    } catch (e) {
        // expected
    }
    // Since we pass "restart: true" here, the node will start with the same options as above unless
    // specified. We do want to wait to be able to connect to the node here however, so we need to
    // pass
    // {waitForConnect: true}.
    priConn = replTest.start(priConn.nodeId, {shardsvr: '', waitForConnect: true}, true);
    priConn.setSlaveOk();

    // Wait for the old primary to replicate the document that was written to the new primary while
    // it was shut down.
    assert.soonNoExcept(function() {
        return priConn.getDB('test').foo.findOne();
    });

    // Ensure that there's no sharding state on the restarted original primary, since the
    // shardIdentity doc should have been rolled back.
    res = priConn.getDB('admin').runCommand({shardingState: 1});
    assert(!res.enabled, tojson(res));
    assert.eq(null, priConn.getDB('admin').system.version.findOne({_id: 'shardIdentity'}));

    replTest.stopSet();

    st.stop();
})();
