/*
 * Tests that the checkShardingIndex command checks shard version when run on a sharded collection.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// checkShardingIndex only exists on a mongod, so run the command directly against a shard with
// a dummy shard version that should fail with StaleConfig.
//
// Note the shell connects to shards with a DBClient, which throws StaleConfig errors as JS
// exceptions when the error does not come from a mongos.
const error = assert.throws(() => {
    st.rs0.getPrimary().getDB(dbName).runCommand({
        checkShardingIndex: ns,
        keyPattern: {x: 1},
        shardVersion: [Timestamp(99, 10101), ObjectId()],
    });
});
assert.eq(error.code, ErrorCodes.StaleConfig);

st.stop();
})();
