/**
 * Verifies that removing a shard and re-adding it with a different shard ID (but the same
 * replica set) works correctly.
 *
 * The test uses two mongoses:
 *   - mongos1 (st.s0): performs the remove + add shard cycle
 *   - mongos2 (st.s1): kept idle so its ShardRegistry cache remains stale, then used for
 *     listDatabases to trigger a refresh
 *
 * @tags: [requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

const st = new ShardingTest({shards: 2, mongos: 2, rs: {nodes: 1}});

const mongos1 = st.s0;
const mongos2 = st.s1;

const kExtraRSName = "extraShard";
const kExtraRSPort = allocatePort();
let rsCounter = 0;
function startFreshExtraRS() {
    // Keep RS name and host:port stable to exercise RSM-name-based behavior while avoiding
    // sharding metadata carry-over via a fresh dbpath each cycle.
    const rs = new ReplSetTest({
        name: kExtraRSName,
        nodes: 1,
        nodeOptions: {
            shardsvr: "",
            port: kExtraRSPort,
            dbpath: MongoRunner.dataPath + kExtraRSName + "_" + rsCounter++,
        },
    });
    rs.startSet({remember: false});
    rs.initiate();
    rs.waitForPrimary();
    return rs;
}

let extraRS = startFreshExtraRS();

let shardName = "extraShard_0";
assert.commandWorked(mongos1.adminCommand({addShard: extraRS.getURL(), name: shardName}));

// Warm mongos2's ShardRegistry cache so it knows about the extra shard.
assert.commandWorked(mongos2.adminCommand({listDatabases: 1}));

// Remove the shard via mongos1 (st routes through st.s = st.s0 = mongos1).
// mongos2 stays idle — its cache still has the old shard ID.
removeShard(st, shardName);

// Stop the old RS and create a fresh RS with the same set name.
extraRS.stopSet(null, true, {remember: false, skipValidation: true});
extraRS = startFreshExtraRS();

// Re-add with a new shard ID but the same underlying replica set name.
const newName = "extraShard_1";
assert.commandWorked(mongos1.adminCommand({addShard: extraRS.getURL(), name: newName}));

assert.commandWorked(mongos2.adminCommand({listShards: 1}));

// Verifies that removing a shard and re-adding it with a different shard ID does not cause HostUnreachable errors on other mongos instances.
const res = mongos2.adminCommand({listDatabases: 1});
assert.commandWorked(res);

// Cleanup via mongos1.
const shards = mongos1.adminCommand({listShards: 1}).shards;
for (const s of shards) {
    if (s.host.startsWith("extraShard/")) {
        removeShard(st, s._id);
    }
}

extraRS.stopSet();
st.stop();
