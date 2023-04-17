/**
 * Shuts down config server and shard replica set nodes one by one and ensures correct behaviour.
 *
 * Restarts the config server, which requires persistence so restarted nodes can rejoin their
 * original replica set and run shutdown hooks.
 * @tags: [requires_persistence]
 */

// Checking UUID and index consistency involves talking to the config servers, which are shut down
// in this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

(function() {
'use strict';

var st = new ShardingTest({
    shards: {
        rs0: {nodes: 2},
    },
    config: 3
});

// The default read concern is local, which is incompatible with secondary reads when the primary is
// down.
st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "available"}});

let count = 0;
jsTest.log('Config nodes up: 3 of 3, shard nodes up: 2 of 2: ' +
           'Insert test data to work with');
assert.commandWorked(st.s0.getDB('TestDB').TestColl.update(
    {_id: 0}, {$inc: {count: 1}}, {upsert: true, writeConcern: {w: 2, wtimeout: 30000}}));
count += 1;
assert.eq([{_id: 0, count}], st.s0.getDB('TestDB').TestColl.find().toArray());

jsTest.log('Config nodes up: 2 of 3, shard nodes up: 2 of 2: ' +
           'Inserts and queries must work');
st.configRS.stop(0, undefined, undefined, {forRestart: true});
st.restartMongos(0);
assert.commandWorked(st.s0.getDB('TestDB').TestColl.update(
    {_id: 0}, {$inc: {count: 1}}, {upsert: true, writeConcern: {w: 2, wtimeout: 30000}}));
count += 1;
assert.eq([{_id: 0, count}], st.s0.getDB('TestDB').TestColl.find().toArray());

if (!TestData.configShard) {
    // For a config shard, the config server is the shard, so we can't have a different number up.
    jsTest.log('Config nodes up: 1 of 3, shard nodes up: 2 of 2: ' +
               'Inserts and queries must work');
    st.configRS.stop(1, undefined, undefined, {forRestart: true});
    st.restartMongos(0);
    assert.commandWorked(st.s0.getDB('TestDB').TestColl.update(
        {_id: 0}, {$inc: {count: 1}}, {upsert: true, writeConcern: {w: 2, wtimeout: 30000}}));
    count += 1;
    assert.eq([{_id: 0, count}], st.s0.getDB('TestDB').TestColl.find().toArray());
}

jsTest.log('Config nodes up: 1 of 3, shard nodes up: 1 of 2: ' +
           'Only queries will work (no shard primary)');
st.rs0.stop(0);
st.restartMongos(0);
st.s0.setSecondaryOk();
assert.eq([{_id: 0, count}], st.s0.getDB('TestDB').TestColl.find().toArray());

if (!TestData.configShard) {
    // For a config shard, the config server is the shard, so we can't have a different number up.
    jsTest.log('Config nodes up: 1 of 3, shard nodes up: 0 of 2: ' +
               'MongoS must start, but no operations will work (no shard nodes available)');
    st.rs0.stop(1);
    st.restartMongos(0);
    assert.throws(function() {
        st.s0.getDB('TestDB').TestColl.find().toArray();
    });
}

jsTest.log('Config nodes up: 0 of 3, shard nodes up: 0 of 2: ' +
           'Metadata cannot be loaded at all, no operations will work');
if (!TestData.configShard) {
    st.configRS.stop(2);
} else if (TestData.configShard) {
    st.configRS.stop(1, undefined, undefined, {forRestart: true});
    // Restart mongos while a config server is still up.
    st.restartMongos(0);
    // After taking down the last config/shard node, no user data operations will work.
    st.configRS.stop(2, undefined, undefined, {forRestart: true});
    assert.throws(function() {
        st.s0.getDB('TestDB').TestColl.find().toArray();
    });
}

// Instead of restarting mongos, ensure it has no metadata
assert.commandWorked(st.s0.adminCommand({flushRouterConfig: 1}));

// Throws transport error first and subsequent times when loading config data, not no primary
for (var i = 0; i < 2; i++) {
    try {
        st.s0.getDB('TestDB').TestColl.findOne();

        // Must always throw
        assert(false);
    } catch (e) {
        printjson(e);

        // Make sure we get a transport error, and not a no-primary error
        assert(e.code == 10276 ||  // Transport error
               e.code == 13328 ||  // Connect error
               e.code == ErrorCodes.HostUnreachable ||
               e.code == ErrorCodes.FailedToSatisfyReadPreference ||
               e.code == ErrorCodes.ReplicaSetNotFound);
    }
}

// Restart two config server nodes to ensure that teardown checks may be executed
st.restartAllConfigServers();
st.stop();
}());
