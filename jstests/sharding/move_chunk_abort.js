/**
 * Ensure that moveChunk is expediently aborted if it fails.
 *
 * @tags: [requires_persistence]
 */

import {moveChunkParallel} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const dbName = "test";
const collName = "user";
const staticMongod = MongoRunner.runMongod({});
const st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 1}}});
const collection = st.s.getDB(dbName).getCollection(collName);

CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: 10}, shard: st.shard0.shardName},
    {min: {_id: 10}, max: {_id: MaxKey}, shard: st.shard1.shardName},
]);

for (let i = 0; i < 20; i++) {
    assert.commandWorked(collection.insertOne({_id: i, x: i}));
}

// The recipient will be stuck running transferMods, it won't know the chunkMigration failed unless it's notified
// by the donor and the migrateThread is interrupted.
const transferModsFp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeRunningXferMods");

const joinMoveChunk = moveChunkParallel(
    staticMongod,
    st.s.host,
    {_id: 1},
    null,
    dbName + "." + collName,
    st.shard1.shardName,
    false /*expectSuccess*/,
);

transferModsFp.wait();

// Perform a collMod directly on the shard's primary to cause the moveChunk to abort.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB(dbName)
        .runCommand({collMod: collName, validationLevel: "off", writeConcern: {w: 1}}),
);

joinMoveChunk();

collection.drop();

st.stop();

MongoRunner.stopMongod(staticMongod);
