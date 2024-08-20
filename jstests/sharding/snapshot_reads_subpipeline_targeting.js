// Verifies mongod uses a versioned routing table to target subpipelines for snapshot reads.
//
// @tags: [
//   requires_sharding,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
        // Disable expiring old chunk history to ensure the transactions are able to read from a
        // shard that has donated a chunk, even if the migration takes longer than the amount of
        // time for which a chunk's history is normally stored (see SERVER-39763).
        configOptions: {
            setParameter: {
                "failpoint.skipExpiringOldChunkHistory": "{mode: 'alwaysOn'}",
                minSnapshotHistoryWindowInSeconds: 600
            }
        },
        rsOptions: {setParameter: {minSnapshotHistoryWindowInSeconds: 600}}
    }
});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const local = db.local;
const foreign = db.foreign;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create local collection, shard it and distribute among shards.
CreateShardedCollectionUtil.shardCollectionWithChunks(local, {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: 0}, shard: st.shard0.shardName},
    {min: {_id: 0}, max: {_id: MaxKey}, shard: st.shard1.shardName},
]);
CreateShardedCollectionUtil.shardCollectionWithChunks(foreign, {a: 1}, [
    {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
    {min: {a: 0}, max: {a: MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(local.insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(local.insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(foreign.insert({a: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(foreign.insert({a: 5}, {writeConcern: {w: "majority"}}));

const pipeline = [
    {$lookup: {from: "foreign", localField: "_id", foreignField: "a", as: "f"}},
    {$sort: {_id: 1}}
];

const opTime = db.runCommand({ping: 1}).operationTime;
const readConcern = {
    level: "snapshot",
    atClusterTime: opTime
};

jsTestLog("Running operations with readConcert: " + tojson(readConcern));

const resBefore = local.aggregate(pipeline, {readConcern: readConcern}).toArray();

// Update collection and move chunk.
const session = db.getMongo().startSession({retryWrites: true});
assert.commandWorked(
    session.getDatabase(dbName).getCollection("foreign").updateOne({a: -5}, {$set: {a: 5}}));
assert.commandWorked(
    db.adminCommand({moveChunk: foreign.getFullName(), find: {a: 5}, to: st.shard0.shardName}));

const resAfter = local.aggregate(pipeline, {readConcern: readConcern}).toArray();
assert.eq(resBefore, resAfter);

st.stop();
