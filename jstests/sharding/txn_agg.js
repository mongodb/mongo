/**
 * @tags: [
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 *   # This test uses a new $_internalSplitPipeline syntax introduced in 8.0.
 *   requires_fcv_80,
 * ]
 */
const st = new ShardingTest({shards: 2});

assert.commandWorked(
    st.s.adminCommand({enableSharding: 'test', primaryShard: st.shard0.shardName}));

assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.user', find: {_id: 0}, to: st.shard1.shardName}));

// Preemptively create the collections in the shard since it is not allowed in transactions.
let coll = st.s.getDB('test').user;
coll.insert({_id: 1});
coll.insert({_id: -1});
coll.remove({});

let unshardedColl = st.s.getDB('test').foo;
unshardedColl.insert({_id: 0});
unshardedColl.remove({});

let session = st.s.startSession();
let sessionDB = session.getDatabase('test');
let sessionColl = sessionDB.getCollection('user');
let sessionUnsharded = sessionDB.getCollection('foo');

// Transactions do not internally retry on StaleDbVersion errors, so we
// ensure the primary shard's cached databaseVersion is fresh before running commands through
// mongos on the unsharded collections.
assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: "test"}));

// passthrough

session.startTransaction();

sessionUnsharded.insert({_id: -1});
sessionUnsharded.insert({_id: 1});
assert.eq(2, sessionUnsharded.find().itcount());

let res = sessionUnsharded.aggregate([{$match: {_id: {$gte: -200}}}]).toArray();
assert.eq(2, res.length, tojson(res));

assert.commandWorked(session.abortTransaction_forTesting());

// merge on mongos

session.startTransaction();

sessionColl.insert({_id: -1});
sessionColl.insert({_id: 1});
assert.eq(2, sessionColl.find().itcount());

res = sessionColl.aggregate([{$match: {_id: {$gte: -200}}}], {allowDiskUse: false}).toArray();
assert.eq(2, res.length, tojson(res));

assert.commandWorked(session.abortTransaction_forTesting());

// merge on shard. This will require the merging shard to open a cursor on itself.
session.startTransaction();

sessionColl.insert({_id: -1});
sessionColl.insert({_id: 1});
assert.eq(2, sessionColl.find().itcount());

res = sessionColl
          .aggregate(
              [{$match: {_id: {$gte: -200}}}, {$_internalSplitPipeline: {mergeType: "anyShard"}}])
          .toArray();
assert.eq(2, res.length, tojson(res));

assert.commandWorked(session.abortTransaction_forTesting());

// Error case: provide a readConcern on an operation which comes in the middle of a transaction.
session.startTransaction();

sessionColl.insert({_id: -1});
assert.eq(1, sessionColl.find().itcount());

const err = assert.throws(
    () => sessionColl.aggregate(
        [{$match: {_id: {$gte: -200}}}, {$_internalSplitPipeline: {mergeType: "anyShard"}}],
        {readConcern: {level: "majority"}}

        ));
assert.eq(err.code, ErrorCodes.InvalidOptions, err);

assert.commandWorked(session.abortTransaction_forTesting());

// Insert some data outside of a transaction.
assert.commandWorked(sessionColl.insert([{_id: -1}, {_id: 0}, {_id: 1}]));

// Run an aggregation which requires merging on a shard as the first operation in a transaction.
const specificShardName = st.shard0.shardName;
session.startTransaction();
assert.eq([{_id: -1}, {_id: 0}, {_id: 1}],
          sessionColl
              .aggregate([
                  {$_internalSplitPipeline: {mergeType: {"specificShard": specificShardName}}},
                  {$sort: {_id: 1}}
              ])
              .toArray());
assert.commandWorked(session.commitTransaction_forTesting());

// Move all of the data to shard 1.
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.user', find: {_id: -1}, to: st.shard1.shardName}));

// Be sure that only one shard will be targeted after the moveChunk.
const pipeline = [
    {$_internalSplitPipeline: {mergeType: {"specificShard": specificShardName}}},
    {$sort: {_id: 1}}
];
const explain = sessionColl.explain().aggregate(pipeline);
assert.eq(Object.keys(explain.shards), [st.shard1.shardName], explain);

// Now run the same aggregation, but again, force shard 0 to be the merger even though it has no
// chunks for the collection.
session.startTransaction();
assert.eq([{_id: -1}, {_id: 0}, {_id: 1}], sessionColl.aggregate(pipeline).toArray());
assert.commandWorked(session.commitTransaction_forTesting());

st.stop();