/**
 * Tests that rolling back the insertion of the shardIdentity document on a shard causes the node
 * rolling it back to shut down.
 * @tags: [requires_persistence, requires_journaling]
 */

(function() {
    "use strict";

    load('jstests/libs/write_concern_util.js');

    var st = new ShardingTest({shards: 1});

    var replTest = new ReplSetTest({nodes: 3});
    var nodes = replTest.startSet({shardsvr: ''});
    replTest.initiate();

    var priConn = replTest.getPrimary();
    var secondaries = replTest.getSecondaries();
    var configConnStr = st.configRS.getURL();

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

    // Restart the original primary so it triggers a rollback of the shardIdentity insert.
    jsTest.log("Restarting original primary");
    priConn = replTest.restart(priConn);

    // Wait until we cannot create a connection to the former primary, which indicates that it must
    // have shut itself down during the rollback.
    jsTest.log("Waiting for original primary to rollback and shut down");
    assert.soon(
        function() {
            try {
                var newConn = new Mongo(priConn.host);
                return false;
            } catch (x) {
                return true;
            }
        },
        function() {
            var oldPriOplog = priConn.getDB('local').oplog.rs.find().sort({$natural: -1}).toArray();
            var newPriOplog =
                newPriConn.getDB('local').oplog.rs.find().sort({$natural: -1}).toArray();
            return "timed out waiting for original primary to shut down after rollback. " +
                "Old primary oplog: " + tojson(oldPriOplog) + "; new primary oplog: " +
                tojson(newPriOplog);
        },
        90000);

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
    priConn = replTest.restart(priConn, {shardsvr: ''});
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
