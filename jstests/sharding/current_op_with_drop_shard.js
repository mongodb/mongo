// Tests that currentOp is resilient to drop shard.
(function() {
    'use strict';

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    // We need the balancer to remove a shard.
    st.startBalancer();

    const mongosDB = st.s.getDB(jsTestName());
    const shardName = st.shard0.shardName;

    var res = st.s.adminCommand({removeShard: shardName});
    assert.commandWorked(res);
    assert.eq('started', res.state);
    assert.soon(function() {
        res = st.s.adminCommand({removeShard: shardName});
        assert.commandWorked(res);
        return ('completed' === res.state);
    }, "removeShard never completed for shard " + shardName);

    assert.commandWorked(mongosDB.currentOp());

    st.stop();
})();
