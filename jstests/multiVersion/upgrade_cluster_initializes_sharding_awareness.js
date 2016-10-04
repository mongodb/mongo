/**
 * Tests that upgrading a cluster from 3.2 to 3.4 initializes sharding awareness on all shards.
 * TODO: This test has no value for the 3.4->3.6 upgrade process, so it should be removed after
 * 3.4 becomes 'last-stable'.
 */

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

(function() {

    "use strict";

    var testCRUD = function(db) {
        assert.writeOK(db.foo.insert({x: 1}));
        assert.writeOK(db.foo.insert({x: -1}));
        assert.writeOK(db.foo.update({x: 1}, {$set: {y: 1}}));
        assert.writeOK(db.foo.update({x: -1}, {$set: {y: 1}}));
        var doc1 = db.foo.findOne({x: 1});
        assert.eq(1, doc1.y);
        var doc2 = db.foo.findOne({x: -1});
        assert.eq(1, doc2.y);

        assert.writeOK(db.foo.remove({x: 1}, true));
        assert.writeOK(db.foo.remove({x: -1}, true));
        assert.eq(null, db.foo.findOne());
    };

    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {binVersion: "3.2"},
            configOptions: {binVersion: "3.2"},
            shardOptions: {binVersion: "3.2"},

            // TODO: SERVER-24163 remove after v3.4
            waitForCSRSSecondaries: false
        }
    });
    st.configRS.awaitReplication();

    var shard0Name = st.shard0.shardName;
    var shard1Name = st.shard1.shardName;

    // check that config.version document gets initialized properly
    var version = st.s.getCollection('config.version').findOne();
    assert.eq(version.minCompatibleVersion, 5);
    assert.eq(version.currentVersion, 6);
    var clusterID = version.clusterId;
    assert.neq(null, clusterID);
    assert.eq(version.excluding, undefined);

    // Setup sharded collection
    assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));
    st.ensurePrimaryShard('sharded', shard0Name);
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'sharded.foo', middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'sharded.foo', find: {x: 1}, to: shard1Name}));

    testCRUD(st.s.getDB('unsharded'));
    testCRUD(st.s.getDB('sharded'));

    // Shut down one of the shards so that the upgraded config server can't upsert the shardIdentity
    // document onto it.
    st.stopMongod(1);

    // upgrade the config servers first
    jsTest.log('upgrading config servers');
    st.upgradeCluster("latest", {upgradeMongos: false, upgradeShards: false});
    st.restartMongoses();

    // Assert that the shardIdentity document gets added to the shard that is still online.
    jsTest.log("Waiting for shard 0 to be marked as sharding aware");
    assert.soonNoExcept(function() {
        var shardDoc = st.s.getDB('config').shards.findOne({_id: shard0Name});
        return shardDoc.state == 1;
    });

    var shard0ShardIdentityDoc =
        st.shard0.getDB('admin').system.version.findOne({_id: 'shardIdentity'});
    assert.neq(null, shard0ShardIdentityDoc);
    assert.eq(shard0Name, shard0ShardIdentityDoc.shardName);
    assert.eq(clusterID, shard0ShardIdentityDoc.clusterId);
    assert.eq(st.configRS.getURL(), shard0ShardIdentityDoc.configsvrConnectionString);

    // Verify that shard 1 is still not marked as shard aware
    var shard1ShardDoc = st.s.getDB('config').shards.findOne({_id: shard1Name});
    assert(!shard1ShardDoc.hasOwnProperty('state'));

    // Now upgrade the shards.  This will restart the shard that was down previously.
    jsTest.log('upgrading shard servers');
    st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false});
    st.restartMongoses();

    testCRUD(st.s.getDB('unsharded'));
    testCRUD(st.s.getDB('sharded'));

    // Assert that the shardIdentity document gets added to the shard that was previously offline.
    jsTest.log("Waiting for shard 1 to be marked as sharding aware");
    assert.soonNoExcept(function() {
        var shardDoc = st.s.getDB('config').shards.findOne({_id: shard1Name});
        return shardDoc.state == 1;
    });

    var shard1ShardIdentityDoc =
        st.shard1.getDB('admin').system.version.findOne({_id: 'shardIdentity'});
    assert.neq(null, shard1ShardIdentityDoc);
    assert.eq(shard1Name, shard1ShardIdentityDoc.shardName);
    assert.eq(clusterID, shard1ShardIdentityDoc.clusterId);
    assert.eq(st.configRS.getURL(), shard1ShardIdentityDoc.configsvrConnectionString);

    // Assert that both shards have their in-memory sharding state properly initialized and that
    // it includes the clusterId.
    var shard0ShardingState = st.shard0.getDB('admin').runCommand({shardingState: 1});
    assert(shard0ShardingState.enabled);
    assert.eq(shard0Name, shard0ShardingState.shardName);
    assert.eq(clusterID, shard0ShardingState.clusterId);
    assert.eq(st.configRS.getURL(), shard0ShardingState.configServer);

    var shard1ShardingState = st.shard1.getDB('admin').runCommand({shardingState: 1});
    assert(shard1ShardingState.enabled);
    assert.eq(shard1Name, shard1ShardingState.shardName);
    assert.eq(clusterID, shard1ShardingState.clusterId);
    assert.eq(st.configRS.getURL(), shard1ShardingState.configServer);

    // Finally, upgrade mongos
    jsTest.log('upgrading mongos servers');
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});
    st.restartMongoses();

    testCRUD(st.s.getDB('unsharded'));
    testCRUD(st.s.getDB('sharded'));

    st.stop();

})();
