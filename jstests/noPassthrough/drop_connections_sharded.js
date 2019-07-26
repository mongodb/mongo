/**
 * verify dropConnections command works for sharded clusters
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
"use strict";

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 3},
    mongos: 1,
});
const mongos = st.s0;
const rst = st.rs0;
const primary = rst.getPrimary();

mongos.adminCommand({multicast: {ping: 0}});

function getConnPoolHosts() {
    const ret = mongos.adminCommand({connPoolStats: 1});
    assert.commandWorked(ret);
    jsTestLog("Connection pool stats by host: " + tojson(ret.hosts));
    return ret.hosts;
}

const cfg = primary.getDB('local').system.replset.findOne();
const memberHost = cfg.members[2].host;
assert.eq(memberHost in getConnPoolHosts(), true);

const removedMember = cfg.members.splice(2, 1);
assert.eq(removedMember[0].host, memberHost);
cfg.version++;

jsTestLog("Reconfiguring to omit " + memberHost);
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));
assert.eq(memberHost in getConnPoolHosts(), true);

jsTestLog("Dropping connections to " + memberHost);
assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
assert.soon(() => {
    return !(memberHost in getConnPoolHosts());
});

// need to re-add removed node or test complain about the replset config
cfg.members.push(removedMember[0]);
cfg.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

st.stop();
})();
