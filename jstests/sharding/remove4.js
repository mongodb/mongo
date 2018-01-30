// Validates that after a primary shard is drained, a new sharded collection will not be created on
// the primary shard
(function() {
    'use strict';

    function removeShardAddNewColl(shardCollCmd) {
        let st = new ShardingTest({name: "remove_shard4", shards: 2, mongos: 2});
        assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
        let primaryShard = st.shard0.shardName;
        st.ensurePrimaryShard('TestDB', primaryShard);

        // Remove primary shard
        var removeRes;
        removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: primaryShard}));
        assert.eq('started', removeRes.state);

        // Add a new sharded collection and check that its data is not on the primary drained shard
        assert.commandWorked(st.s0.adminCommand(shardCollCmd));
        st.s0.getDB('TestDB').Coll.insert({_id: -2, value: 'Negative value'});
        st.s0.getDB('TestDB').Coll.insert({_id: 2, value: 'Positive value'});

        let chunks = st.config.chunks.find({'ns': 'TestDB.Coll'}).toArray();
        assert.neq(chunks.length, 0);

        for (let i = 0; i < chunks.length; i++) {
            assert.neq(chunks[i].shard,
                       primaryShard,
                       'New sharded collection should not have been created on primary shard');
        }

        removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: primaryShard}));
        assert.eq('ongoing', removeRes.state);

        // Drop TestDB so can finish draining
        assert.commandWorked(st.s0.getDB('TestDB').runCommand({dropDatabase: 1}));

        // Move the config.system.sessions chunk off primary
        assert.commandWorked(st.s0.adminCommand({
            moveChunk: 'config.system.sessions',
            find: {_id: 'config.system.sessions-_id_MinKey'},
            to: st.shard1.shardName,
            _waitForDelete: true
        }));

        // Remove shard must succeed now
        removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: primaryShard}));
        assert.eq('completed', removeRes.state);

        st.stop();
    }

    removeShardAddNewColl({shardCollection: 'TestDB.Coll', key: {_id: 1}});
    removeShardAddNewColl(
        {shardCollection: 'TestDB.Coll', key: {_id: "hashed"}, numInitialChunks: 2});
})();
