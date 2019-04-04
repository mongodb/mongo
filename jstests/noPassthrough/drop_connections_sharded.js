/**
 * verify dropConnections command works for sharded clusters
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
    "use strict";

    const st = new ShardingTest({shards: 1, rs0: {nodes: 3}, merizos: 1});
    const merizos = st.s0;
    const rst = st.rs0;
    const primary = rst.getPrimary();

    merizos.adminCommand({multicast: {ping: 0}});

    const cfg = primary.getDB('local').system.replset.findOne();
    const memberHost = cfg.members[2].host;
    const removedMember = cfg.members.splice(2, 1);
    cfg.version++;

    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    assert.eq(1, merizos.adminCommand({connPoolStats: 1}).hosts[memberHost].available);
    assert.commandWorked(merizos.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
    assert.eq(0, merizos.adminCommand({connPoolStats: 1}).hosts[memberHost].available);

    // need to re-add removed node or test complain about the replset config
    cfg.members.push(removedMember[0]);
    cfg.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

    st.stop();
})();
