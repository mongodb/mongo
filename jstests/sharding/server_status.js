/**
 * Basic test for the 'sharding' section of the serverStatus response object for
 * both mongos and the shard.
 */

(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});
    const testDB = st.s.getDB("test");
    const testColl = testDB.coll;

    assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

    // Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
    st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

    // Insert one document on each shard.
    assert.commandWorked(testColl.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl.insert({x: -1, _id: 0}));

    let checkShardingServerStatus = function(doc) {
        let shardingSection = doc.sharding;
        assert.neq(shardingSection, null);

        let configConnStr = shardingSection.configsvrConnectionString;
        let configConn = new Mongo(configConnStr);
        let configIsMaster = configConn.getDB('admin').runCommand({isMaster: 1});

        let configOpTimeObj = shardingSection.lastSeenConfigServerOpTime;

        assert.gt(configConnStr.indexOf('/'), 0);
        assert.gte(configIsMaster.configsvr, 1);  // If it's a shard, this field won't exist.
        assert.neq(null, configOpTimeObj);
        assert.neq(null, configOpTimeObj.ts);
        assert.neq(null, configOpTimeObj.t);

        assert.neq(null, shardingSection.maxChunkSizeInBytes);
    };

    // Update documents using _id in query so that we publish
    // 'updateOneOpStyleBroadcastWithExactIDCount' metric.

    // Should increment the metric as the update cannot target single shard and are {multi:false}.
    assert.commandWorked(testDB.coll.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
    assert.commandWorked(testDB.coll.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

    // Should increment the metric because we broadcast by _id, even though the update subsequently
    // fails on the shard when it attempts an invalid modification of the shard key.
    assert.commandFailedWithCode(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {multi: false}),
                                 [ErrorCodes.ImmutableField, 31025]);

    // Shouldn't increment the metric when {multi:true}.
    assert.commandWorked(testDB.coll.update({_id: 1}, {$set: {a: 3}}, {multi: true}));
    assert.commandWorked(testDB.coll.update({}, {$set: {a: 3}}, {multi: true}));

    // Shouldn't increment the metric when update can target single shard.
    assert.commandWorked(testDB.coll.update({x: 11}, {$set: {a: 2}}, {multi: false}));
    assert.commandWorked(testDB.coll.update({x: 1}, {$set: {a: 2}}, {multi: false}));

    // Shouldn't increment the metric when routing fails.
    assert.commandFailedWithCode(testDB.coll.update({}, {$set: {x: 2}}, {multi: false}),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {upsert: true}),
                                 ErrorCodes.ShardKeyNotFound);

    const mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that only the first three updates incremented the metric counter.
    assert.eq(3, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

    checkShardingServerStatus(mongosServerStatus);

    const mongodServerStatus = st.rs0.getPrimary().getDB('admin').runCommand({serverStatus: 1});
    checkShardingServerStatus(mongodServerStatus);

    st.stop();
})();
