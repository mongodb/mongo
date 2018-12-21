/**
 * verify dropConnections command works for sharded clusters
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
    "use strict";

    const st = new ShardingTest({shards: 1, rs0: {nodes: 3}, mongos: 1});
    const mongos = st.s0;
    const rst = st.rs0;
    const primary = rst.getPrimary();

    mongos.adminCommand({multicast: {ping: 0}});

    const cfg = primary.getDB('local').system.replset.findOne();
    const memberHost = cfg.members[2].host;
    const removedMember = cfg.members.splice(2, 1);
    cfg.version++;

    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    assert.eq(1, mongos.adminCommand({connPoolStats: 1}).hosts[memberHost].available);
    assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
    assert.eq(0, mongos.adminCommand({connPoolStats: 1}).hosts[memberHost].available);

    // need to re-add removed node or test complain about the replset config
    cfg.members.push(removedMember[0]);
    cfg.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    st.stop();
})();
