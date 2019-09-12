/**
 * Tests that a retry of a successful shardCollection will send setShardVersion to the
 * primary shard.
 */

(function() {
    'use strict';

    let st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 2}}});
    const dbName = 'db';
    const coll = 'foo';
    const nss = dbName + '.' + coll;
    const mongos = st.s0;
    const configPrimary = st.configRS.getPrimary();

    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

    assert.commandWorked(configPrimary.getDB('admin').runCommand({
        configureFailPoint: 'skipSendingSetShardVersionAfterCompletionOfShardCollection',
        mode: 'alwaysOn'
    }));

    // Returns true if the shard is aware that the collection is sharded.
    const isShardAware = (shard, coll) => {
        const res =
            assert.commandWorked(shard.adminCommand({getShardVersion: coll, fullMetadata: true}));
        return res.metadata.hasOwnProperty("collVersion");
    };

    assert.commandWorked(mongos.adminCommand({shardCollection: nss, key: {aKey: 1}}));
    assert(!isShardAware(st.rs0.getPrimary(), nss));
    assert.eq(
        configPrimary.getDB('config').chunks.find({ns: nss, shard: st.shard0.shardName}).itcount(),
        1);

    assert.commandWorked(mongos.adminCommand({shardCollection: nss, key: {aKey: 1}}));
    assert(isShardAware(st.rs0.getPrimary(), nss));

    st.stop();
})();
