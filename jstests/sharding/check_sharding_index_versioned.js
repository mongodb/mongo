/**
 * Tests that the checkShardingIndex command checks shard version when run on a sharded collection.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// checkShardingIndex only exists on a mongod, so run the command directly against a shard with
// a dummy shard version that should fail with StaleConfig.
assert.commandFailedWithCode(st.rs0.getPrimary().getDB(dbName).runCommand({
    checkShardingIndex: ns,
    keyPattern: {x: 1},
    shardVersion: {e: ObjectId(), t: Timestamp(1, 1), v: Timestamp(99, 10101)},
}),
                             ErrorCodes.StaleConfig);

st.stop();