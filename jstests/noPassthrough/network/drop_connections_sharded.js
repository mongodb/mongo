/**
 * verify dropConnections command works for sharded clusters
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {
    assertHasConnPoolStats,
    checkHostHasNoOpenConnections,
    checkHostHasOpenConnections
} from "jstests/libs/conn_pool_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 3},
    mongos: 1,
});
const mongos = st.s0;
const rst = st.rs0;
const primary = rst.getPrimary();
let currentCheckNumber = 0;

mongos.adminCommand({multicast: {ping: 0}});

const cfg = primary.getDB('local').system.replset.findOne();
const memberHost = cfg.members[2].host;
currentCheckNumber = assertHasConnPoolStats(
    mongos, [memberHost], {checkStatsFunc: checkHostHasOpenConnections}, currentCheckNumber);

const removedMember = cfg.members.splice(2, 1);
assert.eq(removedMember[0].host, memberHost);
cfg.version++;

jsTestLog("Reconfiguring to omit " + memberHost);
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));
currentCheckNumber = assertHasConnPoolStats(
    mongos, [memberHost], {checkStatsFunc: checkHostHasOpenConnections}, currentCheckNumber);

jsTestLog("Dropping connections to " + memberHost);
assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
currentCheckNumber = assertHasConnPoolStats(
    mongos, [memberHost], {checkStatsFunc: checkHostHasNoOpenConnections}, currentCheckNumber);

// need to re-add removed node or test complain about the replset config
cfg.members.push(removedMember[0]);
cfg.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: cfg}));

st.stop({parallelSupported: false});
