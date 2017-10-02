/**
 * Tests upgrading a cluster from last stable to current version.
 */

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test restarts shards, and the node that is elected primary
// after the restart may be different from the original primary. Since the shell does not retry on
// NotMaster errors, and whether or not it detects the new primary before issuing the command is
// nondeterministic, skip the consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {

    /**
     * @param isRSCluster {bool} use replica set shards.
     */
    var runTest = function(isRSCluster) {
        "use strict";

        jsTest.log("Starting" + (isRSCluster ? " (replica set)" : "") + " cluster" + "...");

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
                mongosOptions: {binVersion: "last-stable"},
                configOptions: {binVersion: "last-stable"},
                shardOptions: {binVersion: "last-stable"},

                rsOptions: {binVersion: "last-stable"},
                rs: isRSCluster
            }
        });
        st.configRS.awaitReplication();

        // check that config.version document gets initialized properly
        var version = st.s.getCollection('config.version').findOne();
        assert.eq(version.minCompatibleVersion, 5);
        assert.eq(version.currentVersion, 6);
        var clusterID = version.clusterId;
        assert.neq(null, clusterID);
        assert.eq(version.excluding, undefined);

        // Setup sharded collection
        assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));
        st.ensurePrimaryShard('sharded', st.shard0.shardName);

        assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: 'sharded.foo', middle: {x: 0}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: 'sharded.foo', find: {x: 1}, to: st.shard1.shardName}));

        testCRUD(st.s.getDB('unsharded'));
        testCRUD(st.s.getDB('sharded'));

        // upgrade the config servers first
        jsTest.log('upgrading config servers');
        st.upgradeCluster("latest", {upgradeMongos: false, upgradeShards: false});
        st.restartMongoses();

        testCRUD(st.s.getDB('unsharded'));
        testCRUD(st.s.getDB('sharded'));

        // Then upgrade the shards.
        jsTest.log('upgrading shard servers');
        st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false});
        st.restartMongoses();

        testCRUD(st.s.getDB('unsharded'));
        testCRUD(st.s.getDB('sharded'));

        // Finally, upgrade mongos
        jsTest.log('upgrading mongos servers');
        st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});
        st.restartMongoses();

        testCRUD(st.s.getDB('unsharded'));
        testCRUD(st.s.getDB('sharded'));

        // Check that version document is unmodified.
        version = st.s.getCollection('config.version').findOne();
        assert.eq(version.minCompatibleVersion, 5);
        assert.eq(version.currentVersion, 6);
        assert.neq(clusterID, version.clusterId);
        assert.eq(version.excluding, undefined);

        st.stop();
    };

    runTest(false);
    runTest(true);

})();
