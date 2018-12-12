(function() {
    "use strict";

    const st = new ShardingTest({shards: 1, rs0: {nodes: 3}, mongos: 1});
    const mongos = st.s0;
    const rst = st.rs0;
    const primary = rst.getPrimary();

    mongos.adminCommand({multicast: {ping: 0}});

    var cfg = primary.getDB('local').system.replset.findOne();
    var memberHost = cfg.members[2].host;
    var removedMember = cfg.members.splice(2, 1);
    cfg.version++;

    primary.adminCommand({replSetReconfig: cfg});

    function getAvailableConnections(stats, host) {
        return stats.hosts[host].available - stats.pools.global[host].available;
    }

    assert.eq(1, getAvailableConnections(mongos.adminCommand({connPoolStats: 1}), memberHost));
    assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
    assert.eq(0, getAvailableConnections(mongos.adminCommand({connPoolStats: 1}), memberHost));

    // need to re-add removed node or test complain about the replset config
    cfg.members.push(removedMember[0]);
    cfg.version++;
    primary.adminCommand({replSetReconfig: cfg});

    st.stop();
})();
