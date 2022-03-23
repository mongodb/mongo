/**
 * This test verifies that if a router running a 5.0 binary sends a shardVersion without a timestamp
 * to a shard running a 5.1 or greater binary, the router will end up refreshing.
 */

// TODO (SERVER-64813): remove this test once 6.0 becomes lastLTS
(function() {
'use strict';

load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster

var kDbName = 'db';
var kShardedNss = kDbName + '.foo';

jsTest.log('Deploying cluster version ' + lastLTSFCV);
var st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    other: {
        mongosOptions: {binVersion: lastLTSFCV},
        configOptions: {binVersion: lastLTSFCV},
        rsOptions: {binVersion: lastLTSFCV},
        rs: {nodes: 2}
    }
});
st.configRS.awaitReplication();
assert.commandWorked(
    st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

jsTest.log('Upgrading FCV to 4.4');
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

jsTest.log('Some Workload under FCV 4.4');
assert.commandWorked(st.s.adminCommand({shardCollection: kShardedNss, key: {i: 1}}));
assert.commandWorked(st.s.getDB(kDbName).foo.insert({i: 5}));

jsTest.log('Upgrading FCV to ' + lastLTSFCV);
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

jsTest.log('Upgrading binaries to ' + latestFCV);
st.upgradeCluster('latest', {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false});

jsTest.log('Checking that the router has stale information');
var collVersion = st.s.getDB(kDbName).foo.getShardVersion();
assert.commandWorked(collVersion);
assert.eq(collVersion.versionTimestamp, null);

assert.eq(1, st.s.getDB(kDbName).foo.find({i: 5}).itcount());

jsTest.log('Checking that the router refreshed its information');
collVersion = st.s.getDB(kDbName).foo.getShardVersion();
assert.commandWorked(collVersion);
assert.neq(collVersion.versionTimestamp, null);

st.stop();
})();
