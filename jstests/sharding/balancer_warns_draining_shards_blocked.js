/*
 * Verifies the balancer emits a warning when a removed shard cannot be drained due to balancing
 * being disabled. There are two cases:
 *  - Balancer is disabled.
 *  - Balancing is disabled for a collection, which has chunks on draining shards.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({
    shards: 2,
    config: 1,
    rs: {nodes: 1},
    other: {
        enableBalancer: false,
    }
});

const mongos = st.s0;
const configDB = st.getDB('config');

const dbName = 'test';
const collName = 'collToDrain';
const ns = dbName + '.' + collName;
const db = st.getDB(dbName);
const coll = db.getCollection(collName);

// Shard collection with shard0 as db primary.
assert.commandWorked(
    mongos.adminCommand({enablesharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

// shard0 owns docs with shard key [MinKey, 0), shard1 owns docs with shard key [0, MaxKey)
assert.commandWorked(st.s.adminCommand(
    {moveRange: ns, min: {x: 0}, max: {x: MaxKey}, toShard: st.shard1.shardName}));

// Check that there are only 2 chunks before starting draining.
const chunksBeforeDrain = findChunksUtil.findChunksByNs(configDB, ns).toArray();
assert.eq(2, chunksBeforeDrain.length);

// Force checks to happen continuously to speedup the test.
const csrsPrimary = st.configRS.getPrimary();
configureFailPoint(csrsPrimary, "forceBalancerWarningChecks");

let awaitRemoveShard = startParallelShell(
    funWithArgs(async function(shardName) {
        const {removeShard} = await import("jstests/sharding/libs/remove_shard_util.js");
        removeShard(db, shardName);
    }, st.shard1.shardName), st.s.port);

// Test warning when the balancer is disabled.
// "Draining of removed shards cannot be completed because the balancer is disabled"
checkLog.containsJson(csrsPrimary, 6434000);

sh.disableBalancing(coll);
st.startBalancer();

// Test warning when the balancer is enabled, but a specific collection is disabled.
// "Draining of removed shards cannot be completed because the balancer is disabled for a collection
// which has chunks in those shards".
checkLog.containsJson(csrsPrimary, 7977400);

// Re-enable balancing to let test terminate.
sh.enableBalancing(coll);
awaitRemoveShard();

st.stop();
