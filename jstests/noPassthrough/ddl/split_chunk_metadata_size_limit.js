/**
 * Verifies that an authoritative splitChunk whose resulting chunk metadata would exceed the maximum
 * BSON object size is rejected up front with BSONObjectTooLarge, instead of being attempted and
 * retried forever. The rejection happens during the coordinator's precondition check, before any
 * critical section is acquired, so the split command fails synchronously and leaves the catalog
 * untouched.
 *
 * @tags: [
 *     featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({shards: 2});

const dbName = jsTestName();
const collName = "foo";
const ns = dbName + "." + collName;
const min = {x: MinKey, y: MinKey};
const max = {x: MaxKey, y: MaxKey};

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1, y: 1}}));

const res = assert.commandWorked(st.s.adminCommand({getShardVersion: ns}));
const versionEpoch = res.versionEpoch;

// Each split point carries a 1MB shard-key value. The coordinator's pessimistic estimate
// (min + max + 3x the split-key bytes) is ~21MB, well over the 16MB BSON limit, so the split
// is rejected during precondition checking.
const bigString = "X".repeat(1024 * 1024); // 1MB
const splitPoints = [];
for (let i = 0; i < 7; i++) {
    splitPoints.push({x: i, y: bigString});
}

const configDB = st.s.getDB("config");
assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));

// Verify the split is rejected with BSONObjectTooLarge.
// We need to run the split on the shard since the split command for mongos doesn't have an option
// to specify multiple split points or number of splits.
assert.commandFailedWithCode(
    st.rs0
        .getPrimary()
        .getDB("admin")
        .runCommand({
            splitChunk: ns,
            from: st.shard0.shardName,
            min: min,
            max: max,
            keyPattern: {x: 1},
            splitKeys: splitPoints,
            epoch: versionEpoch,
        }),
    ErrorCodes.BSONObjectTooLarge,
);

// The rejected split must not have committed anything to the global catalog.
assert.eq(
    1,
    findChunksUtil.countChunksForNs(configDB, ns),
    "rejected split unexpectedly changed the chunk count",
);

// A subsequent valid split still succeeds, proving the aborted split left no critical section held
// on the collection.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB("admin")
        .runCommand({
            splitChunk: ns,
            from: st.shard0.shardName,
            min: min,
            max: max,
            keyPattern: {x: 1},
            splitKeys: [{x: 0, y: 0}],
            epoch: versionEpoch,
        }),
);
assert.eq(2, findChunksUtil.countChunksForNs(configDB, ns));

st.stop();
