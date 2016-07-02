/**
 * Tests upgrading a cluster from last stable to current version.
 */

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

(function() {

    /**
     * @param isRSCluster {bool} use replica set shards.
     */
    var runTest = function(isRSCluster) {
        "use strict";

        jsTest.log("Starting" + (isRSCluster ? " (replica set)" : "") + " cluster" + "...");

        var testCRUD = function(mongos) {
            assert.commandWorked(mongos.getDB('test').runCommand({dropDatabase: 1}));
            assert.commandWorked(mongos.getDB('unsharded').runCommand({dropDatabase: 1}));

            var unshardedDB = mongos.getDB('unshareded');
            assert.commandWorked(unshardedDB.runCommand({insert: 'foo', documents: [{x: 1}]}));
            assert.commandWorked(
                unshardedDB.runCommand({update: 'foo', updates: [{q: {x: 1}, u: {$set: {y: 1}}}]}));
            var doc = unshardedDB.foo.findOne({x: 1});
            assert.eq(1, doc.y);
            assert.commandWorked(
                unshardedDB.runCommand({delete: 'foo', deletes: [{q: {x: 1}, limit: 1}]}));
            doc = unshardedDB.foo.findOne();
            assert.eq(null, doc);

            assert.commandWorked(mongos.adminCommand({enableSharding: 'test'}));
            assert.commandWorked(mongos.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

            var shardedDB = mongos.getDB('shareded');
            assert.commandWorked(shardedDB.runCommand({insert: 'foo', documents: [{x: 1}]}));
            assert.commandWorked(
                shardedDB.runCommand({update: 'foo', updates: [{q: {x: 1}, u: {$set: {y: 1}}}]}));
            doc = shardedDB.foo.findOne({x: 1});
            assert.eq(1, doc.y);
            assert.commandWorked(
                shardedDB.runCommand({delete: 'foo', deletes: [{q: {x: 1}, limit: 1}]}));
            doc = shardedDB.foo.findOne();
            assert.eq(null, doc);
        };

        var st = new ShardingTest({
            shards: 2,
            mongos: 1,
            other: {
                mongosOptions: {binVersion: "last-stable"},
                configOptions: {binVersion: "last-stable"},
                shardOptions: {binVersion: "last-stable"},

                rsOptions: {binVersion: "last-stable"},
                rs: isRSCluster,
                // TODO: SERVER-24163 remove after v3.4
                waitForCSRSSecondaries: false
            }
        });
        st.configRS.awaitReplication();

        var version = st.s.getCollection('config.version').findOne();

        assert.eq(version.minCompatibleVersion, 5);
        assert.eq(version.currentVersion, 6);
        var clusterID = version.clusterId;
        assert.neq(null, clusterID);
        assert.eq(version.excluding, undefined);

        // upgrade everything except for mongos
        st.upgradeCluster("latest", {upgradeMongos: false});
        st.restartMongoses();

        testCRUD(st.s);

        // upgrade mongos
        st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});
        st.restartMongoses();

        // Check that version document is unmodified.
        version = st.s.getCollection('config.version').findOne();
        assert.eq(version.minCompatibleVersion, 5);
        assert.eq(version.currentVersion, 6);
        assert.neq(clusterID, version.clusterId);
        assert.eq(version.excluding, undefined);

        testCRUD(st.s);

        st.stop();
    };

    runTest(false);
    runTest(true);

})();
